#include "anvil/mesh.hpp"

#include <algorithm>
#include "anvil/sort.hpp"

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
    const std::size_t nh = 3 * m.F.size();
    const std::uint64_t N = m.V.size();
    std::vector<std::uint64_t> keys(nh);
    std::vector<std::uint32_t> hes(nh);
    for (std::size_t h = 0; h < nh; ++h) {
        std::uint64_t a = m.F[h / 3][h % 3], b = m.F[h / 3][(h % 3 + 1) % 3];
        keys[h] = std::min(a, b) * N + std::max(a, b);
        hes[h] = std::uint32_t(h);
    }
    radix_sort_pairs(keys, hes, bits_for(N * N));
    auto normal = [&](std::size_t f) {
        const auto& t = m.F[f];
        return normalized(cross(m.V[t[1]] - m.V[t[0]], m.V[t[2]] - m.V[t[0]]));
    };
    std::vector<std::array<std::uint32_t, 2>> out;
    for (std::size_t i = 0; i < nh;) {
        std::size_t j = i + 1;
        while (j < nh && keys[j] == keys[i]) ++j;
        bool feature = angle_rad < 0 || j - i != 2 || dot(normal(hes[i] / 3), normal(hes[i + 1] / 3)) < cos_thr;
        if (feature) out.push_back({std::uint32_t(keys[i] / N), std::uint32_t(keys[i] % N)});  // sorted order
        i = j;
    }
    return out;
}

}  // namespace anvil
