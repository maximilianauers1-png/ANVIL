#include "anvil/geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <sstream>

#include <manifold/manifold.h>
#include <nlohmann/json.hpp>
#include <clipper2/clipper.h>

#include "anvil/sketch.hpp"
#include "anvil/stl.hpp"
#include "anvil/tolerance.hpp"

namespace anvil {

const char* node_kind_name(NodeKind k) {
    switch (k) {
        case NodeKind::Box: return "Box";
        case NodeKind::Cylinder: return "Cylinder";
        case NodeKind::Sphere: return "Sphere";
        case NodeKind::Torus: return "Torus";
        case NodeKind::Boolean: return "Boolean";
        case NodeKind::Transform: return "Transform";
        case NodeKind::ImportedMesh: return "Mesh";
        case NodeKind::Sketch: return "Sketch";
        case NodeKind::Extrude: return "Extrude";
        case NodeKind::Revolve: return "Revolve";
        case NodeKind::Fillet: return "Fillet";
        case NodeKind::EdgeChamfer: return "Chamfer";
        case NodeKind::Shell: return "Shell";
        case NodeKind::LinearPattern: return "LinearPattern";
        case NodeKind::CircularPattern: return "CircularPattern";
        case NodeKind::Loft: return "Loft";
        case NodeKind::Sweep: return "Sweep";
        case NodeKind::StepImport: return "Step";
        case NodeKind::Ref: return "Ref";
        case NodeKind::Solidify: return "Solidify";
        case NodeKind::Deform: return "Deform";
    }
    return "?";
}

const std::vector<ParamSpec>& param_specs(NodeKind k) {
    using dims::kAngle;
    using dims::kLength;
    static const std::vector<ParamSpec> box{
        {"x", kLength, 10, true, false, "size along X"},
        {"y", kLength, 10, true, false, "size along Y"},
        {"z", kLength, 10, true, false, "size along Z (height)"}};
    static const std::vector<ParamSpec> cyl{
        {"d", kLength, 20, true, false, "diameter (bottom)"},
        {"h", kLength, 10, true, false, "height"},
        {"d2", kLength, -1, false, true, "top diameter for cones (omit = d)"},
        {"chamfer_top", kLength, 0, false, true, "45-degree chamfer at the top edge"},
        {"chamfer_bottom", kLength, 0, false, true, "45-degree chamfer at the bottom edge"}};
    static const std::vector<ParamSpec> sph{{"d", kLength, 20, true, false, "diameter"}};
    static const std::vector<ParamSpec> tor{
        {"R", kLength, 20, true, false, "major radius (center to tube center)"},
        {"r", kLength, 5, true, false, "minor radius (tube)"}};
    static const std::vector<ParamSpec> xf{
        {"tx", kLength, 0, false, false, "translation X"},
        {"ty", kLength, 0, false, false, "translation Y"},
        {"tz", kLength, 0, false, false, "translation Z"},
        {"rx", kAngle, 0, false, false, "rotation about X (applied first)"},
        {"ry", kAngle, 0, false, false, "rotation about Y"},
        {"rz", kAngle, 0, false, false, "rotation about Z (applied last)"}};
    using dims::kNone;
    static const std::vector<ParamSpec> extrude{
        {"distance", kLength, 10, true, false, "extrusion length along the sketch normal"},
        {"symmetric", kNone, 0, false, true, "1 = extrude half the distance to each side"}};
    static const std::vector<ParamSpec> revolve{
        {"angle", kAngle, 2 * 3.14159265358979323846, true, false, "revolution angle"},
        {"axis", kNone, 1, false, true, "0 = sketch horizontal axis, 1 = sketch vertical axis"}};
    static const std::vector<ParamSpec> fillet{{"r", kLength, 1, true, false, "fillet radius"}};
    static const std::vector<ParamSpec> chamfer{{"d", kLength, 0.5, true, false, "chamfer distance"}};
    static const std::vector<ParamSpec> shell{{"t", kLength, 1, true, false, "wall thickness (inwards)"}};
    static const std::vector<ParamSpec> linpat{
        {"n", kNone, 3, true, false, "number of copies (including the original)"},
        {"dx", kLength, 10, false, false, "spacing X"},
        {"dy", kLength, 0, false, false, "spacing Y"},
        {"dz", kLength, 0, false, false, "spacing Z"}};
    static const std::vector<ParamSpec> circpat{
        {"n", kNone, 6, true, false, "number of copies (including the original)"},
        {"angle", kAngle, 2 * 3.14159265358979323846, true, false, "total angle about Z"}};
    static const std::vector<ParamSpec> loft{{"ruled", kNone, 0, false, true, "1 = straight (ruled) faces"}};
    static const std::vector<ParamSpec> solidify{
        {"width", kLength, 1, true, false, "wall / trace width"},
        {"height", kLength, 1, true, false, "height along the sketch normal"}};
    static const std::vector<ParamSpec> deform{
        {"sx", kNone, 1, true, false, "stretch along X (factor)"},
        {"sy", kNone, 1, true, false, "stretch along Y (factor)"},
        {"sz", kNone, 1, true, false, "stretch along Z (factor)"},
        {"taper", kNone, 1, true, false, "XY scale at the top relative to the bottom"},
        {"twist", kAngle, 0, false, false, "rotation of the top relative to the bottom (about Z)"},
        {"bend", kAngle, 0, false, false, "bend angle in the XZ plane over the X length"}};
    static const std::vector<ParamSpec> none;
    switch (k) {
        case NodeKind::Extrude: return extrude;
        case NodeKind::Revolve: return revolve;
        case NodeKind::Fillet: return fillet;
        case NodeKind::EdgeChamfer: return chamfer;
        case NodeKind::Shell: return shell;
        case NodeKind::LinearPattern: return linpat;
        case NodeKind::CircularPattern: return circpat;
        case NodeKind::Loft: return loft;
        case NodeKind::Solidify: return solidify;
        case NodeKind::Deform: return deform;
        case NodeKind::Box: return box;
        case NodeKind::Cylinder: return cyl;
        case NodeKind::Sphere: return sph;
        case NodeKind::Torus: return tor;
        case NodeKind::Transform: return xf;
        default: return none;
    }
}

namespace {
const ParamSpec* find_spec(NodeKind k, const std::string& name) {
    for (const auto& s : param_specs(k))
        if (name == s.name) return &s;
    return nullptr;
}
void check_names(NodeKind k, const std::map<std::string, Param>& p) {
    for (const auto& [name, v] : p)
        if (!find_spec(k, name)) {
            std::string valid;
            for (const auto& s : param_specs(k)) valid += std::string(valid.empty() ? "" : ", ") + s.name;
            throw GeometryError(std::string(node_kind_name(k)) + " has no parameter '" + name + "' (valid: " +
                                valid + ")");
        }
}
std::map<std::string, Param> with_defaults(NodeKind k, std::map<std::string, Param> p) {
    for (const auto& s : param_specs(k))
        if (!p.count(s.name) && s.default_value >= 0) p[s.name] = Param{s.default_value, {}};
    return p;
}
}  // namespace

bool is_brep_kind(NodeKind k) {
    switch (k) {
        case NodeKind::Extrude: case NodeKind::Revolve: case NodeKind::Fillet: case NodeKind::EdgeChamfer:
        case NodeKind::Shell: case NodeKind::Loft: case NodeKind::Sweep: case NodeKind::StepImport:
            return true;
        default:
            return false;
    }
}

const std::vector<std::string>& attr_names(NodeKind k) {
    static const std::vector<std::string> edges{"edges"}, faces{"faces"}, step{"path", "sha256"}, ref{"name"},
        solid{"ends", "join"}, none;
    switch (k) {
        case NodeKind::Solidify: return solid;
        case NodeKind::Fillet: case NodeKind::EdgeChamfer: return edges;
        case NodeKind::Shell: return faces;
        case NodeKind::StepImport: return step;
        case NodeKind::Ref: return ref;
        default: return none;
    }
}

NodePtr Node::child(const std::string& e) const {
    auto names = child_names();
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == e) return inputs[i];
    return nullptr;
}
std::vector<std::string> Node::child_names() const {
    if (kind == NodeKind::Boolean) return {"a", "b"};
    if (kind == NodeKind::Loft || kind == NodeKind::Sweep) {
        std::vector<std::string> v;
        for (std::size_t i = 0; i < inputs.size(); ++i) v.push_back(std::to_string(i));
        return v;
    }
    if (inputs.size() == 1) return {"src"};
    return {};
}

NodePtr make_primitive(NodeKind k, std::map<std::string, Param> params) {
    check_names(k, params);
    auto n = std::make_shared<Node>();
    n->kind = k;
    n->params = with_defaults(k, std::move(params));
    return n;
}
NodePtr make_boolean(BoolOp op, NodePtr a, NodePtr b) {
    if (!a || !b) throw GeometryError("boolean needs two operands");
    auto n = std::make_shared<Node>();
    n->kind = NodeKind::Boolean;
    n->op = op;
    n->inputs = {std::move(a), std::move(b)};
    return n;
}
NodePtr make_transform(NodePtr src, std::map<std::string, Param> params) {
    if (!src) throw GeometryError("transform needs a source");
    check_names(NodeKind::Transform, params);
    // Fold consecutive translations-only transforms is NOT done: keeping the
    // tree literal makes the generated Python match what the user did.
    auto n = std::make_shared<Node>();
    n->kind = NodeKind::Transform;
    n->params = with_defaults(NodeKind::Transform, std::move(params));
    n->inputs = {std::move(src)};
    return n;
}
NodePtr make_imported(std::shared_ptr<const Mesh> mesh, std::string path, std::string sha, LengthUnit unit) {
    auto n = std::make_shared<Node>();
    n->kind = NodeKind::ImportedMesh;
    n->mesh = std::move(mesh);
    n->source_path = std::move(path);
    n->source_sha256 = std::move(sha);
    n->source_unit = unit;
    return n;
}

NodePtr make_sketch(std::shared_ptr<const Sketch> sk) {
    if (!sk) throw GeometryError("empty sketch");
    auto n = std::make_shared<Node>();
    n->kind = NodeKind::Sketch;
    n->sketch = std::move(sk);
    return n;
}

NodePtr make_feature(NodeKind k, std::vector<NodePtr> inputs, std::map<std::string, Param> params,
                     std::map<std::string, std::string> attrs) {
    check_names(k, params);
    for (const auto& [name, v] : attrs)
        if (std::find(attr_names(k).begin(), attr_names(k).end(), name) == attr_names(k).end())
            throw GeometryError(std::string(node_kind_name(k)) + " has no attribute '" + name + "'");
    std::size_t need_min = k == NodeKind::Loft || k == NodeKind::Sweep ? 2 : (k == NodeKind::StepImport || k == NodeKind::Ref) ? 0 : 1;
    std::size_t need_max = k == NodeKind::Loft ? 64 : need_min;
    if (inputs.size() < need_min || inputs.size() > need_max)
        throw GeometryError(std::string(node_kind_name(k)) + " needs " + std::to_string(need_min) +
                            (need_max > need_min ? "+" : "") + " input(s), got " + std::to_string(inputs.size()));
    for (const auto& i : inputs)
        if (!i) throw GeometryError(std::string(node_kind_name(k)) + ": missing input");
    auto n = std::make_shared<Node>();
    n->kind = k;
    n->inputs = std::move(inputs);
    n->params = with_defaults(k, std::move(params));
    n->attrs = std::move(attrs);
    return n;
}

NodePtr make_ref(const std::string& name) { return make_feature(NodeKind::Ref, {}, {}, {{"name", name}}); }

NodePtr node_at(const NodePtr& root, const std::string& path) {
    NodePtr cur = root;
    std::size_t i = 0;
    while (i < path.size()) {
        std::size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string e = path.substr(i, j - i);
        NodePtr c = cur->child(e);
        if (!c) throw GeometryError("no child '" + e + "' in " + node_kind_name(cur->kind) + " (path '" + path + "')");
        cur = c;
        i = j + 1;
    }
    return cur;
}

NodePtr with_params(const NodePtr& root, const std::string& path, const std::map<std::string, Param>& params) {
    if (path.empty()) {
        if (param_specs(root->kind).empty())
            throw GeometryError(std::string(node_kind_name(root->kind)) + " has no editable parameters");
        check_names(root->kind, params);
        auto n = std::make_shared<Node>(*root);
        for (const auto& [k, v] : params) n->params[k] = v;
        return n;
    }
    std::size_t j = path.find('/');
    std::string head = path.substr(0, j), rest = j == std::string::npos ? "" : path.substr(j + 1);
    NodePtr c = root->child(head);
    if (!c) throw GeometryError("no child '" + head + "' in " + node_kind_name(root->kind));
    auto n = std::make_shared<Node>(*root);
    auto names = root->child_names();
    auto idx = std::find(names.begin(), names.end(), head) - names.begin();
    n->inputs[idx] = with_params(c, rest, params);
    return n;
}

double resolve(const Param& p, const VariableResolver& vars, Dim expected, const char* name) {
    if (p.expr.empty()) return p.value;
    RtQuantity q;
    try {
        q = eval_expr(p.expr, vars);
    } catch (const ExprError& e) {
        throw GeometryError(std::string("parameter '") + name + "' = \"" + p.expr + "\": " + e.what());
    }
    if (!(q.dim == expected))
        throw GeometryError(std::string("parameter '") + name + "' = \"" + p.expr + "\" is " + dim_name(q.dim) +
                            ", expected " + dim_name(expected) +
                            (q.dim.dimensionless() ? " (add a unit, e.g. '25 mm')" : ""));
    return q.value;
}

// ---------------------------------------------------------------- python
namespace {
std::string py_str(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') o += '\\';
        if (c == '\n') { o += "\\n"; continue; }
        o += c;
    }
    return o + "\"";
}
}  // namespace

std::string param_to_python(const Param& p, Dim d) {
    if (!p.expr.empty()) return py_str(p.expr);
    if (d == dims::kLength) return repr_double(p.value) + "*mm";
    if (d == dims::kAngle) {
        // Python evaluates `x*deg` as x * (pi/180); emit "*deg" only if that is exact.
        // Shortest decimal degree value that reproduces the radians bit-exactly.
        const double k = std::numbers::pi / 180.0;
        for (int digits = 1; digits <= 17; ++digits) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.*g", digits, p.value / k);
            double deg = std::strtod(buf, nullptr);
            if (deg * k == p.value) return repr_double(deg) + "*deg";
        }
        return repr_double(p.value) + "*rad";
    }
    return repr_double(p.value);
}

std::string to_python(const NodePtr& n) {
    std::ostringstream o;
    auto args = [&](NodeKind k, bool skip_defaults) {
        bool first = true;
        for (const auto& s : param_specs(k)) {
            auto it = n->params.find(s.name);
            if (it == n->params.end()) continue;
            if (skip_defaults && it->second.expr.empty() && it->second.value == s.default_value &&
                std::string(s.name) != "d" && std::string(s.name) != "h")
                continue;
            o << (first ? "" : ", ") << s.name << "=" << param_to_python(it->second, s.dim);
            first = false;
        }
    };
    switch (n->kind) {
        case NodeKind::Box: o << "box("; args(n->kind, false); o << ")"; break;
        case NodeKind::Cylinder: o << "cylinder("; args(n->kind, true); o << ")"; break;
        case NodeKind::Sphere: o << "sphere("; args(n->kind, false); o << ")"; break;
        case NodeKind::Torus: o << "torus("; args(n->kind, false); o << ")"; break;
        case NodeKind::Boolean: {
            const char* op = n->op == BoolOp::Union ? " | " : n->op == BoolOp::Difference ? " - " : " & ";
            o << "(" << to_python(n->inputs[0]) << op << to_python(n->inputs[1]) << ")";
            break;
        }
        case NodeKind::Transform: {
            o << "transform(" << to_python(n->inputs[0]);
            for (const auto& s : param_specs(n->kind)) {
                const Param& p = n->params.at(s.name);
                if (p.expr.empty() && p.value == 0.0) continue;
                o << ", " << s.name << "=" << param_to_python(p, s.dim);
            }
            o << ")";
            break;
        }
        case NodeKind::ImportedMesh:
            o << "import_mesh(" << py_str(n->source_path) << ", unit=" << unit_info(n->source_unit).symbol
              << ", sha256=" << py_str(n->source_sha256) << ")";
            break;
        case NodeKind::Sketch:
            o << sketch_to_python(*n->sketch);
            break;
        case NodeKind::Ref:
            o << "ref(" << py_str(n->attrs.at("name")) << ")";
            break;
        case NodeKind::StepImport:
            o << "import_step_solid(" << py_str(n->attrs.at("path")) << ", sha256=" << py_str(n->attrs.at("sha256")) << ")";
            break;
        default: {  // features: fn(inputs..., params..., attrs...)
            static const std::map<NodeKind, const char*> fn{
                {NodeKind::Extrude, "extrude"}, {NodeKind::Revolve, "revolve"}, {NodeKind::Fillet, "fillet"},
                {NodeKind::EdgeChamfer, "chamfer_edges"}, {NodeKind::Shell, "shell"},
                {NodeKind::LinearPattern, "pattern_linear"}, {NodeKind::CircularPattern, "pattern_circular"},
                {NodeKind::Loft, "loft"}, {NodeKind::Sweep, "sweep"}, {NodeKind::Solidify, "solidify"},
                {NodeKind::Deform, "deform"}};
            o << fn.at(n->kind) << "(";
            bool first = true;
            for (const auto& in : n->inputs) o << (first ? "" : ", ") << to_python(in), first = false;
            for (const auto& sp : param_specs(n->kind)) {
                auto it = n->params.find(sp.name);
                if (it == n->params.end()) continue;
                o << (first ? "" : ", ") << sp.name << "=" << param_to_python(it->second, sp.dim);
                first = false;
            }
            for (const auto& [k, v] : n->attrs) o << (first ? "" : ", ") << k << "=" << py_str(v), first = false;
            o << ")";
        }
    }
    return o.str();
}

// ---------------------------------------------------------------- cache
std::shared_ptr<const Shape> ShapeCache::get(const std::string& key) {
    std::lock_guard lk(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) return nullptr;
    it->second.first = ++tick_;
    return it->second.second;
}
void ShapeCache::put(const std::string& key, std::shared_ptr<const Shape> s) {
    std::lock_guard lk(mu_);
    map_[key] = {++tick_, std::move(s)};
    while (map_.size() > max_) {
        auto oldest = std::min_element(map_.begin(), map_.end(),
                                       [](const auto& a, const auto& b) { return a.second.first < b.second.first; });
        map_.erase(oldest);
    }
}
std::size_t ShapeCache::size() const {
    std::lock_guard lk(mu_);
    return map_.size();
}
void ShapeCache::clear() {
    std::lock_guard lk(mu_);
    map_.clear();
}

// ---------------------------------------------------------------- evaluation
int circle_segments(double r, double chord) {
    if (!(r > 0) || !(chord > 0)) throw GeometryError("circle_segments: radius and chord must be positive");
    double x = 1.0 - chord / r;
    int n = x <= -1.0 ? 3 : static_cast<int>(std::ceil(std::numbers::pi / std::acos(x)));
    n = std::max(n, 3);
    return (n + 3) / 4 * 4;
}

namespace {

std::shared_ptr<const Mesh> extract(const manifold::Manifold& m) {
    auto g = m.GetMeshGL64();
    auto mesh = std::make_shared<Mesh>();
    const std::size_t np = g.numProp;
    mesh->V.resize(g.vertProperties.size() / np);
    for (std::size_t i = 0; i < mesh->V.size(); ++i)
        mesh->V[i] = {g.vertProperties[np * i], g.vertProperties[np * i + 1], g.vertProperties[np * i + 2]};
    mesh->F.resize(g.triVerts.size() / 3);
    for (std::size_t i = 0; i < mesh->F.size(); ++i)
        mesh->F[i] = {static_cast<std::uint32_t>(g.triVerts[3 * i]), static_cast<std::uint32_t>(g.triVerts[3 * i + 1]),
                      static_cast<std::uint32_t>(g.triVerts[3 * i + 2])};
    return mesh;
}

std::shared_ptr<const Shape> from_manifold(manifold::Manifold m) {
    if (m.Status() != manifold::Manifold::Error::NoError)
        throw GeometryError("geometry kernel error (Manifold status " + std::to_string(int(m.Status())) + ")");
    auto s = std::make_shared<Shape>();
    s->mesh = extract(m);
    s->solid = std::make_shared<manifold::Manifold>(std::move(m));
    return s;
}

const char* kind_word(NodeKind k) { return node_kind_name(k); }

BrepBackend g_backend;

struct Evaluator {
    const VariableResolver& vars;
    const EvalSettings& set;
    ShapeCache* cache;

    double get(const NodePtr& n, const char* name) {
        auto it = n->params.find(name);
        const ParamSpec* spec = find_spec(n->kind, name);
        if (it == n->params.end()) return spec->default_value;
        double v = resolve(it->second, vars, spec->dim, name);
        if (!std::isfinite(v)) throw GeometryError(std::string(kind_word(n->kind)) + "." + name + " is not finite");
        if (spec->positive && !(v > 0))
            throw GeometryError(std::string(kind_word(n->kind)) + "." + name + " must be > 0 (is " +
                                repr_double(v) + " mm)");
        if (spec->non_negative && v < 0)
            throw GeometryError(std::string(kind_word(n->kind)) + "." + name + " must be >= 0 (is " +
                                repr_double(v) + " mm)");
        return v;
    }

    int ref_depth = 0;
    NodePtr deref(const NodePtr& n) {
        const std::string& name = n->attrs.at("name");
        NodePtr t = set.lookup ? set.lookup(name) : nullptr;
        if (!t) throw GeometryError("reference to unknown object '" + name + "'");
        return t;
    }
    struct RefGuard {
        int& d;
        explicit RefGuard(int& x) : d(x) {
            if (++d > 32) throw GeometryError("references form a cycle (an object refers to itself)");
        }
        ~RefGuard() { --d; }
    };

    // Mesh-only features (Solidify, Deform, imported meshes) have no exact form.
    bool has_mesh_only(const NodePtr& n) {
        if (n->kind == NodeKind::Solidify || n->kind == NodeKind::Deform || n->kind == NodeKind::ImportedMesh) return true;
        if (n->kind == NodeKind::Ref) {
            RefGuard g(ref_depth);
            return has_mesh_only(deref(n));
        }
        for (const auto& c : n->inputs)
            if (has_mesh_only(c)) return true;
        return false;
    }

    // Whole subtree in the B-Rep kernel? Mixed trees (e.g. a filleted part minus
    // a solidified trace) evaluate their exact subtrees in the worker and combine
    // the tessellations with Manifold; only a B-Rep feature applied *on top of*
    // mesh geometry is impossible (brep_json names it).
    bool needs_brep(const NodePtr& n) {
        if (is_brep_kind(n->kind)) return true;
        if (n->kind == NodeKind::Ref) {
            RefGuard g(ref_depth);
            return needs_brep(deref(n));
        }
        if (has_mesh_only(n)) return false;
        for (const auto& c : n->inputs)
            if (needs_brep(c)) return true;
        return false;
    }

    // Canonical key: resolved values, so "2*r" and 25 mm give the same mesh.
    std::string key(const NodePtr& n) {
        if (n->kind == NodeKind::Ref) {
            RefGuard g(ref_depth);
            return key(deref(n));
        }
        std::ostringstream o;
        o << node_kind_name(n->kind) << "|chord=" << repr_double(set.chord_mm);
        for (const auto& [k, v] : n->attrs) o << "|" << k << "=" << v;
        if (n->kind == NodeKind::Sketch) o << "|" << sketch_to_python(solve_sketch(*n->sketch, vars).first);
        if (n->kind == NodeKind::Boolean) o << "|op=" << int(n->op);
        if (n->kind == NodeKind::ImportedMesh) o << "|mesh=" << n->source_sha256 << "|" << n->mesh.get();
        for (const auto& s : param_specs(n->kind))
            if (n->params.count(s.name)) o << "|" << s.name << "=" << repr_double(get(n, s.name));
        for (const auto& c : n->inputs) o << "|(" << key(c) << ")";
        return sha256_hex(o.str());
    }

    std::shared_ptr<const Shape> eval(const NodePtr& n) {
        std::string k = key(n);
        if (cache)
            if (auto hit = cache->get(k)) return hit;
        auto s = n->kind != NodeKind::Sketch && n->kind != NodeKind::Ref && needs_brep(n) ? compute_brep(n) : compute(n);
        if (cache) cache->put(k, s);
        return s;
    }

    std::shared_ptr<const Shape> compute_brep(const NodePtr& n) {
        if (!g_backend) throw GeometryError("the B-Rep kernel (anvil-geom) is not available in this process");
        auto mesh = g_backend(brep_json(n, vars, set), set.chord_mm);
        auto sh = std::make_shared<Shape>();
        sh->brep = true;
        sh->mesh = mesh;
        manifold::MeshGL64 g;
        g.numProp = 3;
        for (const auto& p : mesh->V) g.vertProperties.insert(g.vertProperties.end(), {p.x, p.y, p.z});
        for (const auto& t : mesh->F) g.triVerts.insert(g.triVerts.end(), {t[0], t[1], t[2]});
        manifold::Manifold m(g);
        if (m.Status() == manifold::Manifold::Error::NoError) sh->solid = std::make_shared<manifold::Manifold>(std::move(m));
        else sh->error = "B-Rep tessellation is not a closed manifold (Manifold status " + std::to_string(int(m.Status())) + ")";
        return sh;
    }

    std::shared_ptr<const Shape> compute(const NodePtr& n) {
        using manifold::Manifold;
        switch (n->kind) {
            case NodeKind::Box: {
                double x = get(n, "x"), y = get(n, "y"), z = get(n, "z");
                return from_manifold(Manifold::Cube({x, y, z}).Translate({-x / 2, -y / 2, 0}));
            }
            case NodeKind::Cylinder: {
                double d = get(n, "d"), h = get(n, "h");
                auto it = n->params.find("d2");
                double d2 = it == n->params.end() ? d : get(n, "d2");
                double ct = get(n, "chamfer_top"), cb = get(n, "chamfer_bottom");
                double r0 = d / 2, r1 = d2 / 2;
                if (ct + cb >= h)
                    throw GeometryError("Cylinder: chamfer_top + chamfer_bottom (" + repr_double(ct + cb) +
                                        " mm) must be smaller than h (" + repr_double(h) + " mm)");
                if (cb >= r0 && cb > 0)
                    throw GeometryError("Cylinder: chamfer_bottom must be smaller than d/2 (" + repr_double(r0) + " mm)");
                if (ct > 0 && ct >= r1)
                    throw GeometryError("Cylinder: chamfer_top must be smaller than d2/2 (" + repr_double(r1) + " mm)");
                auto rside = [&](double z) { return r0 + (r1 - r0) * z / h; };
                manifold::SimplePolygon poly{{0, 0}};
                if (cb > 0) poly.push_back({r0 - cb, 0}), poly.push_back({rside(cb), cb});
                else poly.push_back({r0, 0});
                if (ct > 0) poly.push_back({rside(h - ct), h - ct}), poly.push_back({r1 - ct, h});
                else if (r1 > 0) poly.push_back({r1, h});
                poly.push_back({0, h});
                int segs = circle_segments(std::max(r0, r1), set.chord_mm);
                return from_manifold(Manifold::Revolve({poly}, segs));
            }
            case NodeKind::Sphere: {
                // UV sphere as a revolved semicircle. On a doubly curved surface
                // the deviation at a face center is ~ sagitta_u + sagitta_v, so
                // each direction gets chord/2 (guarantee: deviation <= chord).
                double r = get(n, "d") / 2;
                int nm = circle_segments(r, set.chord_mm / 2);
                manifold::SimplePolygon c;
                for (int i = 0; i <= nm / 2; ++i) {
                    double a = -std::numbers::pi / 2 + 2 * std::numbers::pi * i / nm;
                    c.push_back({i == 0 || i == nm / 2 ? 0.0 : r * std::cos(a), r * std::sin(a)});
                }
                return from_manifold(Manifold::Revolve({c}, nm));
            }
            case NodeKind::Torus: {
                double R = get(n, "R"), r = get(n, "r");
                if (r >= R) throw GeometryError("Torus: r (" + repr_double(r) + " mm) must be smaller than R (" +
                                                repr_double(R) + " mm)");
                int nm = circle_segments(r, set.chord_mm / 2);  // doubly curved: chord/2 per direction
                manifold::SimplePolygon c;
                for (int i = 0; i < nm; ++i) {
                    double a = 2 * std::numbers::pi * i / nm;
                    c.push_back({R + r * std::cos(a), r * std::sin(a)});
                }
                return from_manifold(Manifold::Revolve({c}, circle_segments(R + r, set.chord_mm / 2)));
            }
            case NodeKind::Boolean: {
                auto a = eval(n->inputs[0]), b = eval(n->inputs[1]);
                for (auto* s : {&a, &b})
                    if (!(*s)->solid) throw GeometryError("boolean operand is not a closed manifold: " + (*s)->error);
                Manifold r = n->op == BoolOp::Union        ? *a->solid + *b->solid
                             : n->op == BoolOp::Difference ? *a->solid - *b->solid
                                                           : *a->solid ^ *b->solid;
                return from_manifold(std::move(r));
            }
            case NodeKind::Transform: {
                auto s = eval(n->inputs[0]);
                double tx = get(n, "tx"), ty = get(n, "ty"), tz = get(n, "tz");
                const double k = 180.0 / std::numbers::pi;
                double rx = get(n, "rx") * k, ry = get(n, "ry") * k, rz = get(n, "rz") * k;
                if (s->solid) return from_manifold(s->solid->Rotate(rx, ry, rz).Translate({tx, ty, tz}));
                auto out = std::make_shared<Shape>(*s);  // non-manifold mesh: transform vertices directly
                auto mesh = std::make_shared<Mesh>(*s->mesh);
                const double ax = rx / k, ay = ry / k, az = rz / k;
                for (auto& p : mesh->V) {
                    Vec3 q{p.x, p.y * std::cos(ax) - p.z * std::sin(ax), p.y * std::sin(ax) + p.z * std::cos(ax)};
                    q = {q.x * std::cos(ay) + q.z * std::sin(ay), q.y, -q.x * std::sin(ay) + q.z * std::cos(ay)};
                    q = {q.x * std::cos(az) - q.y * std::sin(az), q.x * std::sin(az) + q.y * std::cos(az), q.z};
                    p = q + Vec3{tx, ty, tz};
                }
                out->mesh = mesh;
                return out;
            }
            case NodeKind::Sketch: {
                auto sh = std::make_shared<Shape>();
                auto solved = solve_sketch(*n->sketch, vars);
                sh->mesh = std::make_shared<Mesh>();
                sh->lines = std::make_shared<Mesh>(sketch_lines(solved.first, set.chord_mm, true));
                sh->error = "a sketch is not a solid (extrude or revolve it)";
                return sh;
            }
            case NodeKind::Ref: {
                RefGuard g(ref_depth);
                return eval(deref(n));
            }
            case NodeKind::LinearPattern: case NodeKind::CircularPattern: {
                auto src = eval(n->inputs[0]);
                if (!src->solid) throw GeometryError("pattern source is not a closed manifold: " + src->error);
                int count = int(std::lround(get(n, "n")));
                if (count < 1 || count > 10000) throw GeometryError("pattern count must be 1..10000");
                std::vector<Manifold> parts;
                for (int i = 0; i < count; ++i) {
                    if (n->kind == NodeKind::LinearPattern)
                        parts.push_back(src->solid->Translate({get(n, "dx") * i, get(n, "dy") * i, get(n, "dz") * i}));
                    else {
                        double total = get(n, "angle");
                        bool full = std::abs(total - 2 * std::numbers::pi) < 1e-12;
                        double step = total / (full ? count : std::max(count - 1, 1));
                        parts.push_back(src->solid->Rotate(0, 0, step * i * 180.0 / std::numbers::pi));
                    }
                }
                return from_manifold(Manifold::BatchBoolean(parts, manifold::OpType::Add));
            }
            case NodeKind::Solidify: {
                const NodePtr& skn = n->inputs[0]->kind == NodeKind::Ref ? deref(n->inputs[0]) : n->inputs[0];
                if (skn->kind != NodeKind::Sketch) throw GeometryError("solidify needs a sketch");
                Sketch solved = solve_sketch(*skn->sketch, vars).first;
                double w = get(n, "width"), h = get(n, "height");
                auto attr = [&](const char* k, const char* d) { return n->attrs.count(k) ? n->attrs.at(k) : std::string(d); };
                std::string ends = attr("ends", "round"), join = attr("join", "round");
                Clipper2Lib::JoinType jt = join == "miter" ? Clipper2Lib::JoinType::Miter : join == "square" ? Clipper2Lib::JoinType::Square : Clipper2Lib::JoinType::Round;
                Clipper2Lib::EndType open_end = ends == "square" ? Clipper2Lib::EndType::Square
                                              : ends == "flat" ? Clipper2Lib::EndType::Butt : Clipper2Lib::EndType::Round;
                Clipper2Lib::PathsD open_paths, closed_paths;
                for (const auto& c : sketch_chains(solved, set.chord_mm)) {
                    Clipper2Lib::PathD p;
                    for (auto [x, y] : c.pts) p.push_back({x, y});
                    (c.closed ? closed_paths : open_paths).push_back(p);
                }
                if (open_paths.empty() && closed_paths.empty()) throw GeometryError("solidify: the sketch has no curves");
                // arc tolerance from the chord error: round joins/ends honour the same sagitta bound
                const int precision = 6;
                Clipper2Lib::PathsD out;
                if (!open_paths.empty())
                    out = Clipper2Lib::InflatePaths(open_paths, w / 2, jt, open_end, 2.0, precision, set.chord_mm);
                if (!closed_paths.empty()) {
                    auto ring = Clipper2Lib::InflatePaths(closed_paths, w / 2, jt, Clipper2Lib::EndType::Joined, 2.0, precision, set.chord_mm);
                    out = out.empty() ? ring : Clipper2Lib::Union(out, ring, Clipper2Lib::FillRule::NonZero, precision);
                }
                manifold::Polygons polys;
                for (const auto& p : out) {
                    manifold::SimplePolygon sp;
                    for (const auto& q : p) sp.push_back({q.x, q.y});
                    polys.push_back(sp);
                }
                Manifold m = Manifold::Extrude(polys, h);
                Vec3 o, u, v, nn;
                solved.frame(o, u, v, nn);
                manifold::mat3x4 T({u.x, u.y, u.z}, {v.x, v.y, v.z}, {nn.x, nn.y, nn.z}, {o.x, o.y, o.z});
                return from_manifold(m.Transform(T));
            }
            case NodeKind::Deform: {
                auto src = eval(n->inputs[0]);
                if (!src->solid) throw GeometryError("deform source is not a closed manifold: " + src->error);
                double sx = get(n, "sx"), sy = get(n, "sy"), sz = get(n, "sz"), taper = get(n, "taper");
                double twist = get(n, "twist"), bend = get(n, "bend");
                Manifold m = src->solid->Scale({sx, sy, sz});
                manifold::Box bb = m.BoundingBox();
                const double x0 = bb.min.x, x1 = bb.max.x, z0 = bb.min.z, z1 = bb.max.z;
                const double xc = (bb.min.x + bb.max.x) / 2, yc = (bb.min.y + bb.max.y) / 2;
                const double H = std::max(z1 - z0, 1e-9), W = std::max(x1 - x0, 1e-9);
                if (taper != 1 || twist != 0 || bend != 0) {
                    // refine so the warp is resolved: edges no longer than the chord-based
                    // length that keeps the sagitta of a full turn below the chord error
                    double len = std::max(std::sqrt(8 * set.chord_mm * std::max(W, H)), std::max(W, H) / 400);
                    m = m.RefineToLength(len);
                    const double R = bend != 0 ? W / bend : 0;
                    m = m.Warp([=](manifold::vec3& p) {
                        double t = (p.z - z0) / H;
                        double s = 1 + (taper - 1) * t;
                        double a = twist * t;
                        double dx = (p.x - xc) * s, dy = (p.y - yc) * s;
                        p.x = xc + dx * std::cos(a) - dy * std::sin(a);
                        p.y = yc + dx * std::sin(a) + dy * std::cos(a);
                        if (bend != 0) {  // bend the X extent around an axis parallel to Y below the part
                            double phi = (p.x - x0) / R - bend / 2;
                            double r = R + (p.z - z0);
                            p.x = xc + r * std::sin(phi);
                            p.z = z0 - R + r * std::cos(phi);
                        }
                    });
                }
                return from_manifold(std::move(m));
            }
            case NodeKind::ImportedMesh: {
                manifold::MeshGL64 g;
                g.numProp = 3;
                for (const auto& p : n->mesh->V) g.vertProperties.insert(g.vertProperties.end(), {p.x, p.y, p.z});
                for (const auto& t : n->mesh->F) g.triVerts.insert(g.triVerts.end(), {t[0], t[1], t[2]});
                Manifold m(g);
                if (m.Status() == Manifold::Error::NoError && m.NumTri() > 0) return from_manifold(std::move(m));
                auto s = std::make_shared<Shape>();
                s->mesh = n->mesh;
                s->error = "imported mesh is not a closed 2-manifold (Manifold status " +
                           std::to_string(int(m.Status())) + "); mesh repair arrives in M5";
                return s;
            }
        }
        throw GeometryError("unknown node kind");
    }
};

struct Affine {  // p' = R p + t
    double R[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Vec3 t{};
    Vec3 apply(const Vec3& p) const {
        return Vec3{R[0][0] * p.x + R[0][1] * p.y + R[0][2] * p.z, R[1][0] * p.x + R[1][1] * p.y + R[1][2] * p.z,
                    R[2][0] * p.x + R[2][1] * p.y + R[2][2] * p.z} + t;
    }
    Vec3 dir(const Vec3& p) const { return apply(p) - t; }
};

}  // namespace

void set_brep_backend(BrepBackend b) { g_backend = std::move(b); }
bool has_brep_backend() { return bool(g_backend); }

namespace {
nlohmann::json seg_json(const ProfileSegment& g) {
    return {{"arc", g.arc}, {"a", {g.ax, g.ay}}, {"b", {g.bx, g.by}}, {"c", {g.cx, g.cy}}, {"r", g.r},
            {"ccw", g.ccw}, {"full", g.full_circle}};
}
}  // namespace

std::string brep_json(const NodePtr& root, const VariableResolver& vars, const EvalSettings& set) {
    Evaluator ev{vars, set, nullptr};
    std::function<nlohmann::json(const NodePtr&)> js = [&](const NodePtr& n) -> nlohmann::json {
        nlohmann::json j;
        if (n->kind == NodeKind::Ref) {
            Evaluator::RefGuard g(ev.ref_depth);
            return js(ev.deref(n));
        }
        if (n->kind == NodeKind::ImportedMesh)
            throw GeometryError("an imported mesh cannot be combined with B-Rep features (fillet, extrude, ...); "
                                "mesh-to-solid conversion arrives with the repair tools (M5)");
        if (n->kind == NodeKind::Solidify || n->kind == NodeKind::Deform)
            throw GeometryError(std::string(node_kind_name(n->kind)) +
                                " produces mesh geometry; exact features (fillet, chamfer, shell, extrude, ...) cannot be "
                                "applied on top of it. Apply the exact features first, then combine with the " +
                                node_kind_name(n->kind) + " result");
        std::string kind = node_kind_name(n->kind);
        std::transform(kind.begin(), kind.end(), kind.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        j["k"] = kind;
        nlohmann::json p = nlohmann::json::object();
        for (const auto& s : param_specs(n->kind))
            if (n->params.count(s.name)) p[s.name] = ev.get(n, s.name);
        if (n->kind == NodeKind::Cylinder && !n->params.count("d2")) p["d2"] = ev.get(n, "d");
        j["p"] = p;
        if (n->kind == NodeKind::Boolean) j["op"] = n->op == BoolOp::Union ? "union" : n->op == BoolOp::Difference ? "difference" : "intersection";
        if (!n->attrs.empty()) j["a"] = n->attrs;
        if (n->kind == NodeKind::Sketch) {
            auto solved = solve_sketch(*n->sketch, vars).first;
            Vec3 o, u, v, nn;
            solved.frame(o, u, v, nn);
            j["origin"] = {o.x, o.y, o.z}, j["u"] = {u.x, u.y, u.z}, j["v"] = {v.x, v.y, v.z}, j["n"] = {nn.x, nn.y, nn.z};
            std::string profile_error, path_error;
            try {
                nlohmann::json faces = nlohmann::json::array();
                for (const auto& f : sketch_profiles(solved)) {
                    nlohmann::json fj;
                    for (const auto& g : f.outer.segs) fj["outer"].push_back(seg_json(g));
                    fj["holes"] = nlohmann::json::array();
                    for (const auto& h : f.holes) {
                        nlohmann::json hj = nlohmann::json::array();
                        for (const auto& g : h.segs) hj.push_back(seg_json(g));
                        fj["holes"].push_back(hj);
                    }
                    faces.push_back(fj);
                }
                j["faces"] = faces;
            } catch (const GeometryError& e) {
                profile_error = e.what();
            }
            try {
                nlohmann::json path = nlohmann::json::array();
                for (const auto& g : sketch_path(solved)) path.push_back(seg_json(g));
                j["path"] = path;
            } catch (const GeometryError& e) {
                path_error = e.what();
            }
            j["profile_error"] = profile_error;
            j["path_error"] = path_error;
        }
        nlohmann::json in = nlohmann::json::array();
        for (const auto& c : n->inputs) in.push_back(js(c));
        j["in"] = in;
        return j;
    };
    return js(root).dump();
}

std::shared_ptr<const Shape> evaluate(const NodePtr& n, const VariableResolver& vars, const EvalSettings& s,
                                      ShapeCache* cache) {
    if (!(s.chord_mm >= tol::kMinChordMm))
        throw GeometryError("chord error " + repr_double(s.chord_mm) + " mm is below the minimum " +
                            repr_double(tol::kMinChordMm) + " mm");
    Evaluator ev{vars, s, cache};
    return ev.eval(n);
}

AnalyticFeatures analytic_features(const NodePtr& root, const VariableResolver& vars) {
    AnalyticFeatures out;
    EvalSettings dummy;
    Evaluator ev{vars, dummy, nullptr};
    std::function<void(const NodePtr&, const Affine&)> walk = [&](const NodePtr& n, const Affine& A) {
        auto circle = [&](double r, double z) {
            if (r > 0) out.circles.push_back({A.apply({0, 0, z}), normalized(A.dir({0, 0, 1})), r});
        };
        try {
            switch (n->kind) {
                case NodeKind::Box: {
                    double z = ev.get(n, "z");
                    out.centers.push_back(A.apply({0, 0, 0}));
                    out.centers.push_back(A.apply({0, 0, z / 2}));
                    out.centers.push_back(A.apply({0, 0, z}));
                    break;
                }
                case NodeKind::Cylinder: {
                    double d = ev.get(n, "d"), h = ev.get(n, "h");
                    double d2 = n->params.count("d2") ? ev.get(n, "d2") : d;
                    double ct = ev.get(n, "chamfer_top"), cb = ev.get(n, "chamfer_bottom");
                    out.centers.push_back(A.apply({0, 0, 0}));
                    out.centers.push_back(A.apply({0, 0, h}));
                    circle(d / 2 - cb, 0);
                    circle(d2 / 2 - ct, h);
                    if (cb > 0) circle(d / 2 + (d2 - d) / 2 * cb / h, cb);
                    if (ct > 0) circle(d / 2 + (d2 - d) / 2 * (h - ct) / h, h - ct);
                    break;
                }
                case NodeKind::Sphere: out.centers.push_back(A.apply({0, 0, 0})); break;
                case NodeKind::Torus: {
                    double R = ev.get(n, "R"), r = ev.get(n, "r");
                    out.centers.push_back(A.apply({0, 0, 0}));
                    circle(R + r, 0), circle(R - r, 0), circle(R, r), circle(R, -r);
                    break;
                }
                case NodeKind::Boolean:
                    walk(n->inputs[0], A);
                    walk(n->inputs[1], A);
                    break;
                case NodeKind::Transform: {
                    const double ax = ev.get(n, "rx"), ay = ev.get(n, "ry"), az = ev.get(n, "rz");
                    // R = Rz * Ry * Rx (same convention as Manifold::Rotate)
                    double cx = std::cos(ax), sx = std::sin(ax), cy = std::cos(ay), sy = std::sin(ay),
                           cz = std::cos(az), sz = std::sin(az);
                    double Rm[3][3] = {{cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx},
                                       {sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx},
                                       {-sy, cy * sx, cy * cx}};
                    Affine L;
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j) L.R[i][j] = Rm[i][j];
                    L.t = {ev.get(n, "tx"), ev.get(n, "ty"), ev.get(n, "tz")};
                    Affine C;  // C = A o L
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j) {
                            C.R[i][j] = 0;
                            for (int k = 0; k < 3; ++k) C.R[i][j] += A.R[i][k] * L.R[k][j];
                        }
                    C.t = A.apply(L.t);
                    walk(n->inputs[0], C);
                    break;
                }
                case NodeKind::ImportedMesh: break;
            }
        } catch (const GeometryError&) {
            // invalid parameters: no analytic features (the evaluation reports the error)
        }
    };
    walk(root, Affine{});
    return out;
}

}  // namespace anvil
