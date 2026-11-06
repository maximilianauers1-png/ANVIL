// Feature nodes (immutable DAG) and their evaluation with Manifold (ADR 0003/0004).
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "anvil/expr.hpp"
#include "anvil/mesh.hpp"
#include "anvil/sdf.hpp"
#include "anvil/units.hpp"

namespace manifold { class Manifold; }

namespace anvil {

struct GeometryError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// A parameter value: either a literal (internal units) or an expression that
/// is re-evaluated against the document variables (parametric).
struct Param {
    double value = 0.0;
    std::string expr;  // empty -> literal
    bool operator==(const Param&) const = default;
};

enum class NodeKind {
    // M1 (mesh kernel, Manifold)
    Box, Cylinder, Sphere, Torus, Boolean, Transform, ImportedMesh,
    // M2
    Sketch,        // 2D constrained sketch (no volume; drawn as lines)
    Extrude,       // in: sketch
    Revolve,       // in: sketch
    Fillet,        // in: solid; attr "edges"
    EdgeChamfer,   // in: solid; attr "edges"
    Shell,         // in: solid; attr "faces"
    LinearPattern, CircularPattern,  // in: solid
    Loft,          // in: >= 2 sketches
    Sweep,         // in: profile sketch, path sketch
    StepImport,    // attr "path", "sha256"
    Ref,           // attr "name": the current root of another object (parametric link)
    Solidify,      // in: sketch; thickens every chain to a wall (mesh kernel, Clipper2)
    Deform,        // in: solid; stretch / taper / twist / bend (mesh kernel)
    // M3
    Sculpt,        // in: solid; voxel size + sculpt strokes on its SDF (sparse bricks)
    // more primitives (mesh kernel; z = 0 is the base, centred on X/Y unless noted)
    Tube, Prism, Pyramid, Capsule, RoundedBox, Wedge, Ellipsoid, Star,
    // M4
    Figure,        // parametric figure from an armature (Mini Forge); attr "preset"; evaluates to clay (SDF)
    MiniBase,      // round miniature base with procedural top; attr "texture"; clay (SDF)
};
/// True for the primitives evaluated only by the mesh kernel (no exact form).
bool is_mesh_primitive(NodeKind k);
enum class BoolOp { Union, Difference, Intersection };

const char* node_kind_name(NodeKind k);  // "Box", "Cylinder", ...

/// Description of one parameter of a node kind (drives UI, Python, AI schema).
struct ParamSpec {
    const char* name;
    Dim dim;
    double default_value;
    bool positive;      // must be > 0
    bool non_negative;  // must be >= 0
    const char* help;
};
const std::vector<ParamSpec>& param_specs(NodeKind k);

struct Node;
using NodePtr = std::shared_ptr<const Node>;
struct Sketch;

/// Sculpt node payload: the voxel size and the strokes, in order.
struct SculptData {
    double voxel = 0.05;                 // mm
    std::vector<sdf::Stroke> strokes;
};

/// Kinds that need the exact B-Rep kernel (OpenCASCADE, in the anvil-geom worker).
bool is_brep_kind(NodeKind k);

struct Node {
    NodeKind kind = NodeKind::Box;
    std::map<std::string, Param> params;  // sorted -> canonical
    BoolOp op = BoolOp::Union;
    std::vector<NodePtr> inputs;          // Boolean: {a, b}; Transform: {src}
    // ImportedMesh
    std::shared_ptr<const Mesh> mesh;
    std::string source_path;
    std::string source_sha256;
    LengthUnit source_unit = LengthUnit::Millimeter;
    // M2
    std::map<std::string, std::string> attrs;  // non-numeric attributes (edge selector, ref name, ...)
    std::shared_ptr<const Sketch> sketch;      // Sketch nodes
    std::shared_ptr<const SculptData> sculpt;  // Sculpt nodes

    /// Child by path element: "a"/"b" (Boolean), "src" (Transform, features), "0".."n" (Loft/Sweep).
    NodePtr child(const std::string& elem) const;
    std::vector<std::string> child_names() const;
};

// --- construction helpers (validate parameter names; values checked at eval) ---
NodePtr make_primitive(NodeKind k, std::map<std::string, Param> params);
NodePtr make_boolean(BoolOp op, NodePtr a, NodePtr b);
NodePtr make_transform(NodePtr src, std::map<std::string, Param> params);
NodePtr make_imported(std::shared_ptr<const Mesh> mesh, std::string path, std::string sha256, LengthUnit unit);
NodePtr make_sketch(std::shared_ptr<const Sketch> s);
/// Generic feature node (Extrude, Fillet, ...): validates parameter and attribute names.
NodePtr make_feature(NodeKind k, std::vector<NodePtr> inputs, std::map<std::string, Param> params,
                     std::map<std::string, std::string> attrs);
NodePtr make_ref(const std::string& object_name);
NodePtr make_sculpt(NodePtr src, double voxel_mm, std::vector<sdf::Stroke> strokes);
/// Allowed attribute names per kind.
const std::vector<std::string>& attr_names(NodeKind k);

/// Returns a copy of `root` where the node at `path` ("" = root, "a/b/src")
/// has `params` merged in. Throws GeometryError for unknown paths/params.
NodePtr with_params(const NodePtr& root, const std::string& path, const std::map<std::string, Param>& params);
NodePtr node_at(const NodePtr& root, const std::string& path);

/// Resolves a parameter (literal or expression) to internal units.
double resolve(const Param& p, const VariableResolver& vars, Dim expected, const char* name);

/// Canonical Python source for a node, e.g. "cylinder(d=25*mm, h=3*mm)".
/// Round-trips exactly through the Python API (save/load, journal).
std::string to_python(const NodePtr& n);
std::string param_to_python(const Param& p, Dim d);

// --------------------------------------------------------------- evaluation
struct Circle {
    Vec3 center, normal;
    double radius;
};

/// Analytic features collected from the node tree (for snapping).
struct AnalyticFeatures {
    std::vector<Vec3> centers;
    std::vector<Circle> circles;
};

struct Shape {
    std::shared_ptr<const manifold::Manifold> solid;  // null if the mesh is not a closed manifold
    std::shared_ptr<const Mesh> mesh;                 // always present (empty for sketches)
    std::shared_ptr<const Mesh> lines;                // sketches: line list (pairs of vertices)
    std::string error;                                // why `solid` is null
    bool brep = false;                                // evaluated by the exact B-Rep kernel
    // Sculpt: the field and its chunk meshes (the viewport uploads chunks;
    // `mesh` is their welded union). `solid` is built on demand (booleans).
    std::shared_ptr<const sdf::Grid> grid;
    std::vector<std::shared_ptr<const sdf::ChunkMesh>> chunks;
};

/// Object name -> current root node (for Ref nodes); empty if unknown.
using ObjectLookup = std::function<NodePtr(const std::string&)>;

struct EvalSettings {
    double chord_mm = 0.01;  // see tol::kDefaultChordMm
    ObjectLookup lookup;     // resolves Ref nodes
};

/// B-Rep backend (the anvil-geom worker): JSON description -> mesh. Throws
/// GeometryError with the kernel's message. Installed by the session.
using BrepBackend = std::function<std::shared_ptr<const Mesh>(const std::string& json, double chord_mm)>;
void set_brep_backend(BrepBackend b);
bool has_brep_backend();

/// JSON description of a node tree for the B-Rep kernel: parameters resolved
/// to mm/rad, references inlined, sketches converted to world-space profiles.
std::string brep_json(const NodePtr& n, const VariableResolver& vars, const EvalSettings& s);

/// Content-addressed evaluation cache (thread-safe). Key = SHA-256 of the
/// resolved canonical description + chord, so equal parameters give the
/// same (bit-identical) mesh.
class ShapeCache {
  public:
    explicit ShapeCache(std::size_t max_entries = 256) : max_(max_entries) {}
    std::shared_ptr<const Shape> get(const std::string& key);
    void put(const std::string& key, std::shared_ptr<const Shape> s);
    std::size_t size() const;
    void clear();

  private:
    mutable std::mutex mu_;
    std::size_t max_;
    std::uint64_t tick_ = 0;
    std::unordered_map<std::string, std::pair<std::uint64_t, std::shared_ptr<const Shape>>> map_;
};

/// Number of segments for a full circle of radius r such that the sagitta
/// stays below chord: n = ceil(pi / acos(1 - chord/r)), rounded up to a
/// multiple of 4 (keeps quadrant points exact and the result symmetric).
int circle_segments(double radius_mm, double chord_mm);

std::shared_ptr<const Shape> evaluate(const NodePtr& n, const VariableResolver& vars, const EvalSettings& s,
                                      ShapeCache* cache = nullptr);

/// Registers the grid of a Sculpt node computed elsewhere (the live stroke in
/// the viewport applies the same StrokeApplier steps, so the result is
/// bit-identical): evaluating the node then skips the stroke.
void seed_sculpt_cache(const NodePtr& sculpt_node, const VariableResolver& vars, const EvalSettings& s,
                       std::shared_ptr<const sdf::Grid> grid);

/// The mesh an STL export writes (float32 positions). Validate it with
/// abs_tol_mm = float_tolerance(): sub-ulp slivers of boolean results only
/// touch, and rounding must not make them read as crossings.
Mesh export_mesh(const Shape& s);
AnalyticFeatures analytic_features(const NodePtr& n, const VariableResolver& vars);

}  // namespace anvil
