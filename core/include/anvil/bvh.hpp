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

class Bvh {
  public:
    Bvh() = default;
    explicit Bvh(const Mesh& m);

    bool empty() const { return nodes_.empty(); }
    /// Closest hit with t in (t_min, t_max). Möller–Trumbore, double precision.
    std::optional<RayHit> raycast(const Vec3& origin, const Vec3& dir, double t_min = 0.0,
                                  double t_max = INFINITY) const;
    /// Calls f(tri) for each triangle whose box overlaps `box`.
    void query(const Box3& box, const std::function<void(std::uint32_t)>& f) const;
    const Box3& bounds() const { return nodes_.front().box; }
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
