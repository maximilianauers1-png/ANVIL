// Mini Forge (M4): parametric figures from an armature of round cones,
// blended with a polynomial smooth minimum into a signed distance field
// (M0 plan 3.8). The result is sculptable clay (sparse-brick SDF).
//
// Anatomy is laid out at real scale (1800 mm reference human, facing -Y,
// standing on z = 0) and scaled to the target height: 1.80 m -> 32 mm is
// 1:56.25. Pose angles rotate the limbs about their joints (forward
// kinematics); all of it is plain parameters, so every value is editable and
// scrubbable like any other dimension.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "anvil/mesh.hpp"

namespace anvil::figure {

/// Round cone (capsule with different end radii); a == b gives a sphere.
struct RoundCone {
    Vec3 a, b;
    double ra = 0, rb = 0;
};
/// Thin triangle slab (wing membranes): distance to the triangle minus t.
struct Slab {
    Vec3 a, b, c;
    double t = 0;
};

struct Body {
    std::vector<RoundCone> cones;
    std::vector<Slab> slabs;
    double blend = 0;   // smooth-min radius k (mm, model scale)
    Box3 bounds;        // of all primitives incl. radii (mm, model scale)
    /// Signed distance (mm) of the blended union.
    double sdf(const Vec3& p) const;
};

/// Parameters, all in model units (mm, rad). Missing pose angles are 0.
struct Params {
    std::string preset = "human";  // "human" | "demon"
    double height = 32;            // mm, top of the head (rest pose)
    double build = 0.5;            // 0 slender .. 1 heavy
    double heroic = 0.5;           // 0 realistic .. 1 heroic (bigger head, hands, feet)
    std::map<std::string, double> pose;
    double min_feature = 0;        // mm: smallest radius / half-thickness (>= one voxel keeps the surface manifold)
};

/// Names of the pose angles (degrees in the UI, radians internally).
const std::vector<std::string>& pose_names();

Body make_body(const Params& p);

/// Miniature base: top at z = 0, height h, 45-degree bevel; texture
/// "flagstone" | "rubble" | "plain"; `seed` varies the pattern.
struct Base {
    double d = 25, h = 3.5;
    std::string texture = "flagstone";
    unsigned seed = 1;
    double min_feature = 0;  // mm: groove half-width at least this (manifold contouring)
    double sdf(const Vec3& p) const;
    Box3 bounds() const;
};

/// Exact SDF of a round cone (I. Quilez, "distance functions").
double sd_round_cone(const Vec3& p, const RoundCone& c);
/// Polynomial smooth minimum: h = max(k - |a-b|, 0)/k, min(a,b) - h^2 k/4.
inline double smin(double a, double b, double k) {
    if (k <= 0) return a < b ? a : b;
    const double h = std::max(k - std::abs(a - b), 0.0) / k;
    return std::min(a, b) - h * h * k * 0.25;
}

}  // namespace anvil::figure
