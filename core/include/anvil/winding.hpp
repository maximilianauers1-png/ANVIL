// Generalized winding number of triangle soups (Jacobson, Kavan, Sorkine-
// Hornung 2013, "Robust inside-outside segmentation using generalized winding
// numbers") with the hierarchical dipole approximation of Barill, Dickson,
// Schmidt, Levin, Jacobson 2018 ("Fast winding numbers for soups and clouds").
//
// w(p) = (1 / 4 pi) sum_t Omega_t(p); Omega_t is the signed solid angle of
// triangle t seen from p (Van Oosterom & Strackee 1983):
//   tan(Omega / 2) = a.(b x c) / (|a||b||c| + (a.b)|c| + (b.c)|a| + (c.a)|b|),
// a, b, c = triangle corners minus p. For a closed, outward oriented surface w
// is 1 inside and 0 outside; for AI meshes with holes and overlapping shells
// it is a smooth, robust indicator (threshold |w| > 1/2).
//
// Far field: a cluster with vector area N = sum_t A_t n_t and area-weighted
// centroid c contributes the dipole term N.(c - p) / (4 pi |c - p|^3) (first
// order Taylor term of the solid-angle integral; the zeroth vanishes).
#pragma once
#include <cstdint>
#include <vector>

#include "anvil/mesh.hpp"

namespace anvil {

/// Closes every boundary of `m` by a cone: for each connected component of the
/// boundary 1-chain (edges whose two orientations do not cancel) triangles to
/// the component's length-weighted centroid are added with the opposite
/// orientation. The boundary of a boundary is empty, so the result has no
/// boundary chain and its winding number is integer valued off the surface.
/// Returns the number of added triangles.
std::size_t cap_boundaries(Mesh& m);

class WindingNumber {
  public:
    explicit WindingNumber(const Mesh& m, double beta);
    /// Generalized winding number at p (dimensionless).
    double operator()(const Vec3& p) const;
    /// Exact sum over all triangles (reference for tests).
    double exact(const Vec3& p) const;

  private:
    struct Node {
        Vec3 c;           // area-weighted centroid
        Vec3 n;           // vector area (sum of A_t n_t)
        double r2 = 0;    // squared radius of a ball around c holding the cluster
        std::uint32_t first = 0, count = 0;  // leaf: range in order_
        std::uint32_t left = 0, right = 0;   // inner nodes (count == 0)
    };
    const Mesh& m_;
    double beta2_;
    std::vector<Node> nodes_;
    std::vector<std::uint32_t> order_;
    std::uint32_t build(std::uint32_t first, std::uint32_t count, std::vector<Vec3>& cent);
};

/// Signed solid angle of triangle (a, b, c) seen from the origin.
double solid_angle(const Vec3& a, const Vec3& b, const Vec3& c);

}  // namespace anvil
