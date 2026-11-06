// The `_anvil` module: thin bridge from the Python API (python/anvil) to the
// C++ core. All functions run on the Session's Python thread.
#include <pybind11/embed.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <filesystem>

#include "anvil/geometry.hpp"
#include "anvil/sketch.hpp"
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
    if (s == "tube") return NodeKind::Tube;
    if (s == "prism") return NodeKind::Prism;
    if (s == "pyramid") return NodeKind::Pyramid;
    if (s == "capsule") return NodeKind::Capsule;
    if (s == "rounded_box") return NodeKind::RoundedBox;
    if (s == "wedge") return NodeKind::Wedge;
    if (s == "ellipsoid") return NodeKind::Ellipsoid;
    if (s == "star") return NodeKind::Star;
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
    auto st = S().document().current();
    return evaluate(n.node, st->resolver(), st->eval_settings(chord), &S().cache());
}

// ---- sketches as plain Python structures (the Python Sketch class edits these) ----
py::dict sketch_to_dict(const Sketch& s) {
    py::dict d, pts, crv, con;
    d["plane"] = std::string(plane_name(s.plane));
    d["offset"] = s.offset;
    for (const auto& [id, p] : s.points) pts[py::str(id)] = py::make_tuple(p.x, p.y);
    for (const auto& [id, c] : s.curves) {
        const char* k = c.kind == CurveKind::Line ? "line" : c.kind == CurveKind::Circle ? "circle" : "arc";
        crv[py::str(id)] = py::make_tuple(k, c.pts, c.radius, c.construction);
    }
    for (const auto& [id, c] : s.constraints)
        con[py::str(id)] = py::make_tuple(std::string(constraint_name(c.kind)), c.refs,
                                          py::make_tuple(c.value.value, c.value.expr),
                                          py::make_tuple(c.value2.value, c.value2.expr));
    d["points"] = pts, d["curves"] = crv, d["constraints"] = con;
    return d;
}

Sketch sketch_from_dict(const py::dict& d) {
    Sketch s;
    auto plane = parse_plane(d["plane"].cast<std::string>());
    if (!plane) throw std::invalid_argument("sketch plane must be XY, XZ or YZ");
    s.plane = *plane;
    s.offset = d.contains("offset") ? d["offset"].cast<double>() : 0.0;
    for (auto [k, v] : d["points"].cast<py::dict>()) {
        auto t = v.cast<py::tuple>();
        s.points[k.cast<std::string>()] = {t[0].cast<double>(), t[1].cast<double>()};
    }
    for (auto [k, v] : d["curves"].cast<py::dict>()) {
        auto t = v.cast<py::tuple>();
        SketchCurve c;
        std::string kind = t[0].cast<std::string>();
        c.kind = kind == "line" ? CurveKind::Line : kind == "circle" ? CurveKind::Circle
                 : kind == "arc" ? CurveKind::Arc : throw std::invalid_argument("unknown curve kind '" + kind + "'");
        c.pts = t[1].cast<std::vector<std::string>>();
        c.radius = t.size() > 2 ? t[2].cast<double>() : 0.0;
        c.construction = t.size() > 3 && t[3].cast<bool>();
        s.curves[k.cast<std::string>()] = c;
    }
    for (auto [k, v] : d["constraints"].cast<py::dict>()) {
        auto t = v.cast<py::tuple>();
        auto kind = parse_constraint(t[0].cast<std::string>());
        if (!kind) throw std::invalid_argument("unknown constraint '" + t[0].cast<std::string>() + "'");
        SketchConstraint c;
        c.kind = *kind;
        c.refs = t[1].cast<std::vector<std::string>>();
        if (t.size() > 2) {
            auto a = t[2].cast<py::tuple>();
            c.value = {a[0].cast<double>(), a[1].cast<std::string>()};
        }
        if (t.size() > 3) {
            auto b = t[3].cast<py::tuple>();
            c.value2 = {b[0].cast<double>(), b[1].cast<std::string>()};
        }
        s.constraints[k.cast<std::string>()] = c;
    }
    return s;
}

py::dict status_dict(const SolveStatus& st) {
    py::dict d;
    d["converged"] = st.converged;
    d["dof"] = st.dof;
    d["residual"] = st.residual;
    d["message"] = st.message;
    d["entities"] = st.entity_state;
    d["constraints"] = st.constraint_state;
    return d;
}

NodeKind feature_kind(const std::string& k) {
    static const std::map<std::string, NodeKind> m{
        {"extrude", NodeKind::Extrude}, {"revolve", NodeKind::Revolve}, {"fillet", NodeKind::Fillet},
        {"chamfer", NodeKind::EdgeChamfer}, {"shell", NodeKind::Shell}, {"linear_pattern", NodeKind::LinearPattern},
        {"circular_pattern", NodeKind::CircularPattern}, {"loft", NodeKind::Loft}, {"sweep", NodeKind::Sweep},
        {"step", NodeKind::StepImport}, {"solidify", NodeKind::Solidify}, {"deform", NodeKind::Deform}};
    auto it = m.find(k);
    if (it == m.end()) throw std::invalid_argument("unknown feature '" + k + "'");
    return it->second;
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
        .def("same", [](const NodeHandle& a, const NodeHandle& b) { return a.node == b.node; })
        .def("sculpt_info", [](const NodeHandle& n) {
            if (n.node->kind != NodeKind::Sculpt) throw std::invalid_argument("not a sculpt node");
            py::list strokes;
            for (const auto& s : n.node->sculpt->strokes) {
                py::list path;
                for (const auto& p : s.path) path.append(py::make_tuple(p.x, p.y, p.z));
                py::dict opt;
                if (s.opt.invert) opt["invert"] = true;
                if (s.opt.symmetry >= 0) opt["symmetry"] = std::string(1, "xyz"[s.opt.symmetry]), opt["sym_at"] = s.opt.sym_at;
                if (s.opt.falloff != 1.0) opt["falloff"] = s.opt.falloff;
                strokes.append(py::make_tuple(std::string(sdf::brush_name(s.brush)), s.radius, s.strength, path, opt));
            }
            return py::make_tuple(n.node->sculpt->voxel, strokes);
        });

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
    // --- M2: sketches, features, references, STEP ---------------------------------
    m.def("make_sketch", [](const py::dict& d) { return NodeHandle{make_sketch(std::make_shared<Sketch>(sketch_from_dict(d)))}; });
    m.def("sketch_dict", [](const NodeHandle& n) {
        if (n.node->kind != NodeKind::Sketch) throw std::invalid_argument("not a sketch");
        return sketch_to_dict(*n.node->sketch);
    });
    m.def("sketch_solve", [](const NodeHandle& n) {
        if (n.node->kind != NodeKind::Sketch) throw std::invalid_argument("not a sketch");
        auto [solved, st] = solve_sketch(*n.node->sketch, S().document().current()->resolver());
        return py::make_tuple(NodeHandle{make_sketch(std::make_shared<Sketch>(solved))}, status_dict(st));
    });
    m.def("make_feature", [](const std::string& kind, const std::vector<NodeHandle>& in, const ParamDict& p,
                             const std::map<std::string, std::string>& attrs) {
        std::vector<NodePtr> inputs;
        for (const auto& h : in) inputs.push_back(h.node);
        return NodeHandle{make_feature(feature_kind(kind), inputs, to_params(p), attrs)};
    });
    m.def("make_ref", [](const std::string& name) { return NodeHandle{make_ref(name)}; });
    // --- M3: sculpting -------------------------------------------------------------
    // strokes: [(brush, radius_mm, strength, [(x, y, z), ...]), ...]
    m.def("make_sculpt", [](const NodeHandle& src, double voxel, const py::list& strokes) {
        std::vector<sdf::Stroke> out;
        for (const auto& it : strokes) {
            auto t = it.cast<py::tuple>();
            if (t.size() != 4 && t.size() != 5)
                throw std::invalid_argument("stroke: (brush, radius, strength, path[, options])");
            sdf::Stroke s;
            if (!sdf::parse_brush(t[0].cast<std::string>(), s.brush))
                throw std::invalid_argument("unknown brush '" + t[0].cast<std::string>() +
                                            "' (draw, carve, inflate, clay, crease, pinch, smooth, grab, flatten)");
            s.radius = t[1].cast<double>();
            s.strength = t[2].cast<double>();
            for (const auto& p : t[3].cast<py::list>()) {
                auto q = p.cast<py::tuple>();
                s.path.push_back({q[0].cast<double>(), q[1].cast<double>(), q[2].cast<double>()});
            }
            if (t.size() == 5 && !t[4].is_none()) {
                auto o = t[4].cast<py::dict>();
                for (const auto& [k, v] : o) {
                    const std::string key = k.cast<std::string>();
                    if (key == "invert") s.opt.invert = v.cast<bool>();
                    else if (key == "falloff") s.opt.falloff = v.cast<double>();
                    else if (key == "sym_at") s.opt.sym_at = v.cast<double>();
                    else if (key == "symmetry") {
                        const std::string ax = v.is_none() ? "" : v.cast<std::string>();
                        s.opt.symmetry = ax == "x" ? 0 : ax == "y" ? 1 : ax == "z" ? 2 : ax.empty() ? -1
                                         : throw std::invalid_argument("symmetry must be 'x', 'y', 'z' or None");
                    } else throw std::invalid_argument("unknown stroke option '" + key + "' (invert, symmetry, sym_at, falloff)");
                }
                if (!(s.opt.falloff > 0)) throw std::invalid_argument("falloff must be > 0");
            }
            out.push_back(std::move(s));
        }
        return NodeHandle{make_sculpt(src.node, voxel, std::move(out))};
    });
    m.attr("DEFAULT_VOXEL_MM") = tol::kDefaultVoxelMm;
    m.attr("MIN_VOXEL_MM") = tol::kMinVoxelMm;
    m.def("export_step", [](const NodeHandle& n, const std::string& path) {
        if (S().replaying()) return;
        auto st = S().document().current();
        std::string json = brep_json(n.node, st->resolver(), st->eval_settings(tol::kDefaultChordMm));
        S().geom().export_step(json, upath(path));
    });
    m.def("geom_starts", [] { return S().geom().starts(); });
    m.def("geom_debug_crash", [] { S().geom().debug_crash(); });

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
        d["manifold"] = bool(s->solid) || (s->grid && !s->mesh->F.empty());  // DC output: closed by construction, export re-validates
        d["error"] = s->error;
        return d;
    });
    m.def("check", [](const NodeHandle& n, double chord) {
        auto s = eval_node(n, chord);
        Mesh w = export_mesh(*s);
        return report_dict(validate(w, {.abs_tol_mm = float_tolerance(w)}));
    });
    m.def("export_stl", [](const NodeHandle& n, const std::string& path, const std::string& unit, double chord,
                           bool ascii) {
        auto s = eval_node(n, chord);
        // Validate exactly what will be written: float32 positions (STL stores float).
        Mesh written = export_mesh(*s);
        ValidationReport rep = validate(written, {.abs_tol_mm = float_tolerance(written)});
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
