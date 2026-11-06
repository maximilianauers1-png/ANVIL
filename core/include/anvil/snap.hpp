// Intelligent snapping (M0 plan §3.3).
//
// Candidate i with world point x_i scores J_i = |pi(x_i) - m| / r_i + beta_i in
// screen space (pixels), valid only if |pi(x_i) - m| <= r_i. The lowest J wins;
// hysteresis: a new winner replaces the previous one only if
// J_new < J_old - Delta, so the marker does not flicker.
#pragma once
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "anvil/bvh.hpp"
#include "anvil/camera.hpp"
#include "anvil/geometry.hpp"

namespace anvil {

enum class SnapType : int {
    Vertex, Origin, Intersection, Midpoint, Center, Centroid, Tangent, Perpendicular,
    Axis, Normal, Grid, Plane, None, Count
};
const char* snap_label(SnapType t);  // English UI label, e.g. "Midpoint"

struct SnapTypeSettings {
    bool enabled = true;
    double radius_px = 14;  // r_i (multiplied by the DPI factor)
    double beta = 0;        // priority bias
};

struct SnapSettings {
    std::array<SnapTypeSettings, int(SnapType::Count)> types;
    double hysteresis = 0.15;  // Delta
    double grid_mm = 1.0;
    double dpi_scale = 1.0;
    SnapSettings();  // defaults documented in docs/M0_PLAN.md §3.3
};

/// Geometry of one object prepared for snapping (built off the GUI thread).
struct SnapObject {
    std::string name;
    std::shared_ptr<const Mesh> mesh;
    std::shared_ptr<const Bvh> bvh;
    std::unordered_set<std::uint64_t> feature_keys;  // min*N+max of feature edges
    AnalyticFeatures analytic;
    Vec3 centroid;
};
std::shared_ptr<SnapObject> make_snap_object(std::string name, std::shared_ptr<const Mesh> mesh,
                                             AnalyticFeatures af);

struct SnapScene {
    std::vector<std::shared_ptr<const SnapObject>> objects;
};

struct SnapResult {
    SnapType type = SnapType::None;
    Vec3 point;            // world position of the snapped point
    Vec3 normal{0, 0, 1};  // surface normal at the point (Normal/Plane: plane normal)
    Vec3 axis_dir;         // Axis: direction of the constraint line
    double px = 0, py = 0; // screen position of the point
    double score = 0;
    std::string object;    // object hit under the cursor (may be empty)
    std::string label;     // what to show next to the marker
};

struct SnapQuery {
    Camera cam;
    double mx = 0, my = 0;           // mouse in pixels
    std::optional<Vec3> base;        // base point of a two-point operation
    bool disabled = false;           // Alt held: raw surface/plane point
    std::string ignore_object;       // e.g. the object being moved
    std::vector<std::string> ignore_objects;  // more of them (several objects moved together)
    bool ignores(const std::string& n) const {
        if (n == ignore_object) return true;
        for (const auto& o : ignore_objects)
            if (o == n) return true;
        return false;
    }
};

class SnapEngine {
  public:
    SnapSettings settings;
    SnapResult update(const SnapScene& scene, const SnapQuery& q);
    void reset() { last_.reset(); }

  private:
    std::optional<SnapResult> last_;
};

/// The 26 ViewCube / axis directions: all d in {-1,0,1}^3 \ {0}, normalized.
const std::array<Vec3, 26>& cube_directions();
/// Classifies a point on the surface of the cube [-1,1]^3 into one of the 26
/// regions (face center, edge, corner). Each edge band is `edge_fraction` of
/// the face side length wide (coordinates beyond 1 - 2*edge_fraction). Returns the unnormalized direction in {-1,0,1}^3.
std::array<int, 3> classify_cube_hit(const Vec3& p, double edge_fraction = 0.2);

}  // namespace anvil
