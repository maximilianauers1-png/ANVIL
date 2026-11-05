#include "anvil/mesh.hpp"

#include <algorithm>
#include <unordered_map>

namespace anvil {

Box3 Mesh::bounds() const {
    Box3 b;
    for (const auto& p : V) b.add(p);
    return b;
}

double Mesh::signed_volume() const {
    // Kahan-free but pairwise-safe enough: sum per triangle in double.
    double s = 0.0;
    for (const auto& t : F) s += dot(V[t[0]], cross(V[t[1]], V[t[2]]));
    return s / 6.0;
}

double Mesh::area() const {
    double s = 0.0;
    for (const auto& t : F) s += norm(cross(V[t[1]] - V[t[0]], V[t[2]] - V[t[0]]));
    return s / 2.0;
}

Vec3 Mesh::centroid() const {
    // Centroid of the solid: sum over tetrahedra (0, v0, v1, v2) of vol_i * (v0+v1+v2)/4.
    double vol = 0.0;
    Vec3 c{};
    for (const auto& t : F) {
        double vi = dot(V[t[0]], cross(V[t[1]], V[t[2]])) / 6.0;
        vol += vi;
        c += (V[t[0]] + V[t[1]] + V[t[2]]) * (vi / 4.0);
    }
    return vol != 0.0 ? c / vol : bounds().center();
}

Mesh Mesh::rounded_to_float() const {
    Mesh r = *this;
    for (auto& p : r.V) p = {double(float(p.x)), double(float(p.y)), double(float(p.z))};
    return r;
}

void Mesh::append(const Mesh& o) {
    const auto off = static_cast<std::uint32_t>(V.size());
    V.insert(V.end(), o.V.begin(), o.V.end());
    F.reserve(F.size() + o.F.size());
    for (const auto& t : o.F) F.push_back({t[0] + off, t[1] + off, t[2] + off});
}

std::vector<std::array<std::uint32_t, 2>> feature_edges(const Mesh& m, double angle_rad) {
    const double cos_thr = std::cos(angle_rad);
    std::vector<Vec3> fn(m.F.size());
    for (std::size_t i = 0; i < m.F.size(); ++i) {
        const auto& t = m.F[i];
        fn[i] = normalized(cross(m.V[t[1]] - m.V[t[0]], m.V[t[2]] - m.V[t[0]]));
    }
    // key = min*N + max -> (first face, count, second face)
    struct E { std::uint32_t f0, f1, n; };
    std::unordered_map<std::uint64_t, E> edges;
    edges.reserve(m.F.size() * 2);
    const std::uint64_t N = m.V.size();
    for (std::uint32_t i = 0; i < m.F.size(); ++i) {
        for (int k = 0; k < 3; ++k) {
            std::uint32_t a = m.F[i][k], b = m.F[i][(k + 1) % 3];
            std::uint64_t key = std::uint64_t(std::min(a, b)) * N + std::max(a, b);
            auto [it, inserted] = edges.try_emplace(key, E{i, 0, 1});
            if (!inserted) {
                it->second.f1 = i;
                ++it->second.n;
            }
        }
    }
    std::vector<std::array<std::uint32_t, 2>> out;
    for (const auto& [key, e] : edges) {
        bool feature = e.n != 2 || dot(fn[e.f0], fn[e.f1]) < cos_thr;
        if (feature) out.push_back({std::uint32_t(key / N), std::uint32_t(key % N)});
    }
    std::sort(out.begin(), out.end());  // deterministic order
    return out;
}

}  // namespace anvil
