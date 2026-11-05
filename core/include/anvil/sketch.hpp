// 2D sketches with geometric constraints (M0 plan §3.6, ADR 0016).
//
// Solver: minimise ||C(q)||^2 with Levenberg-Marquardt,
//   (J^T J + lambda D) dq = -J^T C,   D = diag(J^T J)  (Marquardt scaling:
// q mixes mm and radians, plain lambda*I would be unit dependent).
// Degrees of freedom from rank(J) via SVD at the solution: DOF = n - rank.
// Variables with a component in the null space of J are under-determined;
// constraints with a component in the left null space are redundant, and
// conflicting if the residual does not vanish.
#pragma once
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "anvil/expr.hpp"
#include "anvil/geometry.hpp"
#include "anvil/mesh.hpp"

namespace anvil {

struct SketchPoint {
    double x = 0, y = 0;
};

enum class CurveKind { Line, Circle, Arc };

struct SketchCurve {
    CurveKind kind = CurveKind::Line;
    // Line: a, b. Circle: center (p[0]) + radius. Arc: center, start, end (counter-clockwise).
    std::vector<std::string> pts;
    double radius = 0;          // Circle only (unknown of the solver)
    bool construction = false;  // construction geometry is not part of profiles
};

enum class ConstraintKind {
    Coincident, Horizontal, Vertical, Distance, HDistance, VDistance, LineLength, Radius, Diameter,
    LineAngle, Parallel, Perpendicular, Tangent, Equal, Fix, PointOnLine, PointOnCircle, Midpoint,
    Symmetric, Concentric
};
const char* constraint_name(ConstraintKind k);  // "coincident", "distance", ...
std::optional<ConstraintKind> parse_constraint(const std::string& s);
/// True for dimensional constraints that carry a value.
bool constraint_has_value(ConstraintKind k);

struct SketchConstraint {
    ConstraintKind kind = ConstraintKind::Coincident;
    std::vector<std::string> refs;  // point / curve ids
    Param value;                    // Length (mm) or Angle (rad); Fix: x in value, y in value2
    Param value2;
};

enum class SketchPlane { XY, XZ, YZ };

/// Immutable value type (stored in the feature DAG; edits create copies).
struct Sketch {
    SketchPlane plane = SketchPlane::XY;
    double offset = 0;  // mm along the plane normal
    std::map<std::string, SketchPoint> points;     // "P1", "P2", ...
    std::map<std::string, SketchCurve> curves;     // "L1", "C1", "A1", ...
    std::map<std::string, SketchConstraint> constraints;  // "K1", ...

    std::string next_id(char prefix) const;
    /// Plane frame: world = origin + u*x + v*y (right-handed, normal = u x v).
    void frame(Vec3& origin, Vec3& u, Vec3& v, Vec3& n) const;
    Vec3 to_world(double x, double y) const;
};

const char* plane_name(SketchPlane p);
std::optional<SketchPlane> parse_plane(const std::string& s);

struct SolveStatus {
    bool converged = false;
    int iterations = 0;
    double residual = 0;      // max |C_i| (mm or rad)
    int dof = 0;              // remaining degrees of freedom
    int variables = 0, equations = 0, rank = 0;
    std::map<std::string, std::string> entity_state;      // id -> "fixed" | "free"
    std::map<std::string, std::string> constraint_state;  // id -> "ok" | "redundant" | "conflict"
    std::string message;      // English, precise
};

/// Solves the sketch (constraint values resolved with `vars`), starting from
/// the stored positions. Returns the solved sketch and the analysis.
std::pair<Sketch, SolveStatus> solve_sketch(const Sketch& s, const VariableResolver& vars);

/// Closed profile loops of the (solved) sketch, as 2D segments.
struct ProfileSegment {
    bool arc = false;
    double ax = 0, ay = 0, bx = 0, by = 0;  // traversal start / end
    double cx = 0, cy = 0, r = 0;           // arc: center, radius (> 0)
    bool ccw = true;                        // arc traversal direction from a to b
    bool full_circle = false;
    /// Signed sweep angle of the arc (rad, counter-clockwise positive).
    double sweep() const;
    /// Point at parameter t in [0,1] along the segment.
    void at(double t, double& x, double& y) const;
};
struct ProfileLoop {
    std::vector<ProfileSegment> segs;
    double area = 0;  // signed (counter-clockwise > 0)
};
struct ProfileFace {
    ProfileLoop outer;
    std::vector<ProfileLoop> holes;
};
/// Throws GeometryError with a precise message for open or branching profiles.
std::vector<ProfileFace> sketch_profiles(const Sketch& solved);

/// The non-construction curves as one open chain (sweep path), in order.
/// Throws GeometryError if they do not form a single open chain.
std::vector<ProfileSegment> sketch_path(const Sketch& solved);

/// Polyline approximation of every non-construction curve (for display),
/// world coordinates, as a line list in Mesh.V (F unused).
Mesh sketch_lines(const Sketch& solved, double chord_mm, bool include_construction);

/// Canonical Python for a sketch literal (round-trips through the API).
std::string sketch_to_python(const Sketch& s);

}  // namespace anvil
