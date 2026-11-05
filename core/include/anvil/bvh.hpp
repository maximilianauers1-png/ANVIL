// Axis-aligned bounding volume hierarchy over the triangles of a Mesh.
// Used for picking, snapping and the self-intersection check (ADR 0015).
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "anvil/mesh.hpp"

namespace anvil {

struct RayHit {
    std::uint32_t tri = 0;
    double t = 0.0;  // ray parameter: point = origin + t * dir
    Vec3 point;
    Vec3 normal;     // geometric unit normal of the hit triangle
};

/// Closest point on the mesh. feature: 0 = face interior, 1..3 = edge
/// (v0v1, v1v2, v2v0), 4..6 = vertex v0, v1, v2 of triangle `tri`.
struct ClosestHit {
    std::uint32_t tri = 0;
    Vec3 point;
    double dist2 = 0.0;
    int feature = 0;
};
/// Closest point on triangle (a, b, c) to p (Ericson, Real-Time Collision
/// Detection, 5.1.5), with the feature it lies on.
Vec3 closest_point_on_triangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c, int& feature);

class Bvh {
  public:
    Bvh() = default;
    explicit Bvh(const Mesh& m);

    bool empty() const { return nodes_.empty(); }
    /// Closest hit with t in (t_min, t_max). Möller–Trumbore, double precision.
    std::optional<RayHit> raycast(const Vec3& origin, const Vec3& dir, double t_min = 0.0,
                                  double t_max = INFINITY) const;
    /// Closest point within max_dist (nullopt if none). Branch-and-bound on box distances.
    std::optional<ClosestHit> closest(const Vec3& p, double max_dist = INFINITY) const;
    /// Calls f(tri) for each triangle whose box overlaps `box`.
    void query(const Box3& box, const std::function<void(std::uint32_t)>& f) const;
    const Box3& bounds() const {
        static const Box3 empty_box;
        return nodes_.empty() ? empty_box : nodes_.front().box;
    }
    std::size_t node_count() const { return nodes_.size(); }

  private:
    struct Node {
        Box3 box;
        std::uint32_t first = 0;  // leaf: first index into order_; inner: left child
        std::uint32_t count = 0;  // leaf: number of triangles; inner: 0
        std::uint32_t right = 0;  // inner: right child
    };
    const Mesh* mesh_ = nullptr;
    std::vector<Node> nodes_;
    std::vector<std::uint32_t> order_;
};

}  // namespace anvil
