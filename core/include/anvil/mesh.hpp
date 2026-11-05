// Indexed triangle meshes (double precision, mm) and small vector math.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace anvil {

struct Vec3 {
    double x = 0, y = 0, z = 0;
    constexpr Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator/(double s) const { return {x / s, y / s, z / s}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    constexpr Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    constexpr double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    constexpr bool operator==(const Vec3&) const = default;
};
constexpr double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double norm(const Vec3& a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalized(const Vec3& a) {
    double n = norm(a);
    return n > 0 ? a / n : Vec3{};
}
inline Vec3 vmin(const Vec3& a, const Vec3& b) { return {std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z)}; }
inline Vec3 vmax(const Vec3& a, const Vec3& b) { return {std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z)}; }

struct Box3 {
    Vec3 lo{INFINITY, INFINITY, INFINITY};
    Vec3 hi{-INFINITY, -INFINITY, -INFINITY};
    void add(const Vec3& p) { lo = vmin(lo, p); hi = vmax(hi, p); }
    void add(const Box3& b) { lo = vmin(lo, b.lo); hi = vmax(hi, b.hi); }
    bool empty() const { return lo.x > hi.x; }
    Vec3 center() const { return (lo + hi) * 0.5; }
    double diagonal() const { return empty() ? 0.0 : norm(hi - lo); }
    bool overlaps(const Box3& b) const {
        return lo.x <= b.hi.x && hi.x >= b.lo.x && lo.y <= b.hi.y && hi.y >= b.lo.y &&
               lo.z <= b.hi.z && hi.z >= b.lo.z;
    }
};

using Tri = std::array<std::uint32_t, 3>;

struct Mesh {
    std::vector<Vec3> V;
    std::vector<Tri> F;

    std::size_t num_tris() const { return F.size(); }
    Box3 bounds() const;
    /// Signed volume V = (1/6) sum v0 . (v1 x v2)  (divergence theorem).
    double signed_volume() const;
    double area() const;
    /// Volume centroid (center of mass for uniform density).
    Vec3 centroid() const;
    /// Rounds positions to float32 -- what an STL file actually stores.
    Mesh rounded_to_float() const;
    void append(const Mesh& o);
};

/// Edges where the dihedral angle exceeds `angle_rad` (plus boundary edges),
/// as vertex index pairs. Used for rendering outlines and snapping.
std::vector<std::array<std::uint32_t, 2>> feature_edges(const Mesh& m, double angle_rad);

}  // namespace anvil
