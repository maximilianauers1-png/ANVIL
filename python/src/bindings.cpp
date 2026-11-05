// The `_anvil` module: thin bridge from the Python API (python/anvil) to the
// C++ core. All functions run on the Session's Python thread.
#include <pybind11/embed.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <filesystem>

#include "anvil/geometry.hpp"
#include "anvil/stl.hpp"
#include "anvil/tolerance.hpp"
#include "anvil/validate.hpp"
#include "anvil/version.hpp"
#include "session.hpp"

namespace py = pybind11;
using namespace anvil;

namespace {

struct NodeHandle {
    NodePtr node;
};

// UTF-8 std::string (from Python) -> filesystem path, correct on Windows.
std::filesystem::path upath(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

Session& S() {
    Session* s = Session::current();
    if (!s) throw std::runtime_error("no ANVIL session on this thread");
    return *s;
}

Document& doc_for_write() {
    Session& s = S();
    if (s.preview_mode()) throw std::runtime_error("preview: the document cannot be changed while previewing");
    if (!s.document().in_transaction())
        throw std::runtime_error("document changes must run as a command (inside the command bus)");
    return s.document();
}

using ParamDict = std::map<std::string, std::pair<double, std::string>>;
std::map<std::string, Param> to_params(const ParamDict& d) {
    std::map<std::string, Param> m;
    for (const auto& [k, v] : d) m[k] = Param{v.first, v.second};
    return m;
}

NodeKind kind_from(const std::string& s) {
    if (s == "box") return NodeKind::Box;
    if (s == "cylinder") return NodeKind::Cylinder;
    if (s == "sphere") return NodeKind::Sphere;
    if (s == "torus") return NodeKind::Torus;
    throw std::invalid_argument("unknown primitive '" + s + "'");
}

LengthUnit unit_from(const std::string& s) {
    auto u = parse_length_unit(s);
    if (!u) throw std::invalid_argument("unknown length unit '" + s + "' (mm, cm, m, in)");
    return *u;
}

Dim dim_from(const std::string& s) {
    if (s == "Length") return dims::kLength;
    if (s == "Angle") return dims::kAngle;
    if (s == "Dimensionless") return dims::kNone;
    throw std::invalid_argument("unsupported variable dimension '" + s + "'");
}

py::dict report_dict(const ValidationReport& r) {
    py::dict d;
    d["ok"] = r.ok;
    d["summary"] = r.summary();
    d["volume"] = r.volume;
    d["area"] = r.area;
    d["triangles"] = r.triangles;
    d["seconds"] = r.seconds;
    py::list issues;
    for (const auto& i : r.issues) {
        py::dict x;
        x["kind"] = issue_name(i.kind);
        x["message"] = i.message;
        x["index"] = i.index;
        x["location"] = py::make_tuple(i.location.x, i.location.y, i.location.z);
        issues.append(x);
    }
    d["issues"] = issues;
    py::list shells;
    for (const auto& s : r.shells) {
        py::dict x;
        x["faces"] = s.faces;
        x["genus"] = s.genus;
        x["volume"] = s.volume;
        shells.append(x);
    }
    d["shells"] = shells;
    return d;
}

std::shared_ptr<const Shape> eval_node(const NodeHandle& n, double chord) {
    EvalSettings es;
    es.chord_mm = chord;
    return evaluate(n.node, S().document().current()->resolver(), es, &S().cache());
}

}  // namespace

PYBIND11_EMBEDDED_MODULE(_anvil, m) {
    m.doc() = "ANVIL core bridge (internal; use the `anvil` package)";
    m.attr("API_VERSION") = kApiVersion;
    m.attr("DOCUMENT_FORMAT") = kDocumentFormat;
    m.attr("DEFAULT_CHORD_MM") = tol::kDefaultChordMm;
    m.attr("MIN_CHORD_MM") = tol::kMinChordMm;
    m.def("version", [] { return std::string(version_string()); });

    py::register_exception<GeometryError>(m, "GeometryError", PyExc_ValueError);
    py::register_exception<ExprError>(m, "ExpressionError", PyExc_ValueError);

    py::class_<NodeHandle>(m, "Node")
        .def_property_readonly("kind", [](const NodeHandle& n) { return std::string(node_kind_name(n.node->kind)); })
        .def("to_python", [](const NodeHandle& n) { return to_python(n.node); })
        .def("params", [](const NodeHandle& n) {
            ParamDict d;
            for (const auto& [k, v] : n.node->params) d[k] = {v.value, v.expr};
            return d;
        })
        .def("children", [](const NodeHandle& n) { return n.node->child_names(); })
        .def("child", [](const NodeHandle& n, const std::string& path) { return NodeHandle{node_at(n.node, path)}; })
        .def("same", [](const NodeHandle& a, const NodeHandle& b) { return a.node == b.node; });

    m.def("make_primitive", [](const std::string& kind, const ParamDict& p) {
        return NodeHandle{make_primitive(kind_from(kind), to_params(p))};
    });
    m.def("make_boolean", [](const std::string& op, const NodeHandle& a, const NodeHandle& b) {
        BoolOp o = op == "union" ? BoolOp::Union : op == "difference" ? BoolOp::Difference
                   : op == "intersection" ? BoolOp::Intersection
                                         : throw std::invalid_argument("unknown boolean '" + op + "'");
        return NodeHandle{make_boolean(o, a.node, b.node)};
    });
    m.def("make_transform", [](const NodeHandle& src, const ParamDict& p) {
        return NodeHandle{make_transform(src.node, to_params(p))};
    });
    m.def("with_params", [](const NodeHandle& n, const std::string& path, const ParamDict& p) {
        return NodeHandle{with_params(n.node, path, to_params(p))};
    });
    m.def("import_mesh", [](const std::string& path, const std::string& unit, const std::string& sha) {
        std::filesystem::path p = upath(path);
        std::string bytes = read_file(p);
        std::string h = sha256_hex(bytes);
        if (!sha.empty() && sha != h)
            throw std::runtime_error("'" + path + "' changed since it was imported (sha256 " + h.substr(0, 12) +
                                     "... != " + sha.substr(0, 12) + "...)");
        auto r = decode_stl(bytes, unit_from(unit));
        if (r.mesh.F.empty()) throw std::runtime_error("'" + path + "' contains no triangles");
        auto mesh = std::make_shared<Mesh>(std::move(r.mesh));
        return std::make_tuple(NodeHandle{make_imported(mesh, path, h, unit_from(unit))}, h, r.raw_triangles,
                               r.dropped_degenerate);
    });

    // --- document (mutations require an open transaction) -----------------------
    m.def("add_object", [](const std::string& name, const NodeHandle& n, bool visible) {
        return doc_for_write().add_object(name, n.node, visible);
    });
    m.def("remove_object", [](const std::string& name) { doc_for_write().remove_object(name); });
    m.def("set_visible", [](const std::string& name, bool v) { doc_for_write().set_visible(name, v); });
    m.def("rename", [](const std::string& a, const std::string& b) { doc_for_write().rename(a, b); });
    m.def("set_root", [](const std::string& name, const NodeHandle& n) { doc_for_write().set_root(name, n.node); });
    m.def("set_var", [](const std::string& name, double v, const std::string& expr, const std::string& dim) {
        doc_for_write().set_var(name, Param{v, expr}, dim_from(dim));
    });
    m.def("edit", [](const std::string& name, const std::string& path, const ParamDict& p) {
        doc_for_write().edit_params(name, path, to_params(p));
    });
    m.def("clear", [] { doc_for_write().clear(); });
    m.def("objects", [] {
        std::vector<std::pair<std::string, bool>> out;
        for (const auto& o : S().document().current()->objects) out.emplace_back(o->name, o->visible);
        return out;
    });
    m.def("get", [](const std::string& name) {
        const Object* o = S().document().current()->find(name);
        if (!o) throw py::key_error("no object named '" + name + "'");
        return NodeHandle{o->root};
    });
    m.def("vars", [] {
        std::map<std::string, std::pair<double, std::string>> out;
        auto st = S().document().current();
        auto res = st->resolver();
        for (const auto& [k, v] : st->vars) out[k] = {res(k)->value, v.expr};
        return out;
    });
    m.def("document_python", [] { return S().document().current()->to_python(); });
    m.def("unique_name", [](const std::string& prefix) { return S().document().current()->unique_name(prefix); });

    // --- evaluation / checks / export ----------------------------------------------
    m.def("evaluate_info", [](const NodeHandle& n, double chord) {
        auto s = eval_node(n, chord);
        py::dict d;
        Box3 b = s->mesh->bounds();
        d["triangles"] = s->mesh->F.size();
        d["volume"] = s->mesh->signed_volume();
        d["area"] = s->mesh->area();
        d["bbox"] = py::make_tuple(py::make_tuple(b.lo.x, b.lo.y, b.lo.z), py::make_tuple(b.hi.x, b.hi.y, b.hi.z));
        d["manifold"] = bool(s->solid);
        d["error"] = s->error;
        return d;
    });
    m.def("check", [](const NodeHandle& n, double chord) {
        auto s = eval_node(n, chord);
        return report_dict(validate(s->mesh->rounded_to_float()));
    });
    m.def("export_stl", [](const NodeHandle& n, const std::string& path, const std::string& unit, double chord,
                           bool ascii) {
        auto s = eval_node(n, chord);
        // Validate exactly what will be written: float32 positions (STL stores float).
        Mesh written = s->mesh->rounded_to_float();
        ValidationReport rep = validate(written);
        py::dict d = report_dict(rep);
        d["path"] = path;
        d["unit"] = unit;
        d["chord"] = chord;
        if (!rep.ok) {
            std::string msg = "not printable, nothing was written: " + rep.summary();
            for (const auto& i : rep.issues) msg += "\n  - " + i.message;
            throw std::runtime_error(msg);
        }
        if (S().replaying()) {
            d["skipped"] = "replay";
            return d;
        }
        StlWriteOptions o;
        o.ascii = ascii;
        o.unit = unit_from(unit);
        o.note = "chord=" + repr_double(chord) + "mm";
        std::string sha = write_stl(upath(path), *s->mesh, o);
        d["sha256"] = sha;
        d["bytes"] = std::filesystem::file_size(upath(path));
        return d;
    });
    m.def("save_document", [](const std::string& path) { S().save(upath(path)); });

    // --- session state ------------------------------------------------------------------
    m.def("set_preview", [](const NodeHandle& n) { S().set_preview(n.node); });
    m.def("preview_mode", [] { return S().preview_mode(); });
    m.def("replaying", [] { return S().replaying(); });
    m.def("ui_request", [](const std::string& json) {
        if (S().ui_handler && !S().replaying() && !S().preview_mode()) S().ui_handler(json);
    });
    m.def("eval_expr", [](const std::string& text) {
        RtQuantity q = eval_expr(text, S().document().current()->resolver());
        return std::make_tuple(q.value, dim_name(q.dim));
    });
}

// Referenced from session.cpp so the linker keeps this translation unit (and
// with it the static registration of the embedded module) in static builds.
namespace anvil {
void register_embedded_module() {}
}  // namespace anvil
