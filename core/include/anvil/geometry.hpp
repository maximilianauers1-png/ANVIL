// Feature nodes (immutable DAG) and their evaluation with Manifold (ADR 0003/0004).
#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "anvil/expr.hpp"
#include "anvil/mesh.hpp"
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

enum class NodeKind { Box, Cylinder, Sphere, Torus, Boolean, Transform, ImportedMesh };
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

    /// Child by path element: "a"/"b" (Boolean), "src" (Transform).
    NodePtr child(const std::string& elem) const;
    std::vector<std::string> child_names() const;
};

// --- construction helpers (validate parameter names; values checked at eval) ---
NodePtr make_primitive(NodeKind k, std::map<std::string, Param> params);
NodePtr make_boolean(BoolOp op, NodePtr a, NodePtr b);
NodePtr make_transform(NodePtr src, std::map<std::string, Param> params);
NodePtr make_imported(std::shared_ptr<const Mesh> mesh, std::string path, std::string sha256, LengthUnit unit);

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
    std::shared_ptr<const Mesh> mesh;                 // always present
    std::string error;                                // why `solid` is null
};

struct EvalSettings {
    double chord_mm = 0.005;  // see tol::kDefaultChordMm
};

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
AnalyticFeatures analytic_features(const NodePtr& n, const VariableResolver& vars);

}  // namespace anvil
