// Accessories for minis (M4): parametric weapons as signed distance fields.
//
// Frame: the centre of the grip at the origin, the grip along Z, the blade
// (or head) towards +Z, the blade's flat in the XZ plane (edges towards +-X).
// Blades and heads are convex hulls (SDF = max of the face-plane distances:
// exact inside and on the surface, a lower bound outside -- fine for the
// sparse-brick builder, which only needs 1-Lipschitz values); grips, hafts
// and pommels are round cones (figure.hpp).
#pragma once
#include <array>
#include <string>
#include <vector>

#include "anvil/figure.hpp"
#include "anvil/mesh.hpp"

namespace anvil::weapon {

/// Convex polyhedron as the intersection of half-spaces n.p <= d.
struct Hull {
    std::vector<std::array<double, 4>> planes;  // nx, ny, nz, d (unit normals, outwards)
    Box3 box;
    /// Convex hull of a point set (brute force over point triples; n <= ~40).
    static Hull of(const std::vector<Vec3>& pts);
    double sdf(const Vec3& p) const;
};

struct Params {
    std::string type = "longsword";  // see types()
    double length = 19;     // overall length (mm)
    double width = 1.6;     // blade width / axe head height / shield diameter
    double thickness = 0.6; // blade thickness at the spine
    double grip_d = 1.0;    // grip / haft diameter
    double min_feature = 0; // mm: thinnest edge (half the printable edge thickness)
};

/// longsword, zweihander, buster, dagger, axe, spear.
const std::vector<std::string>& types();
/// Default parameters of a type for a figure of the given height (32 mm
/// heroic scale proportions; scaled linearly).
Params defaults(const std::string& type, double figure_height_mm);

struct Weapon {
    std::vector<Hull> hulls;
    std::vector<figure::RoundCone> cones;
    Box3 bounds;
    double sdf(const Vec3& p) const;
};
Weapon make(const Params& p);

}  // namespace anvil::weapon
