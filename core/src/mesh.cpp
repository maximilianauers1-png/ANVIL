#include "anvil/mesh.hpp"

#include <algorithm>
#include "anvil/sort.hpp"

#include <cmath>
#include <numbers>
#include <numeric>
#include <vector>

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

namespace {
struct UnionFind {
    std::vector<std::uint32_t> p;
    explicit UnionFind(std::size_t n) : p(n) { std::iota(p.begin(), p.end(), 0u); }
    std::uint32_t find(std::uint32_t x) {
        while (p[x] != x) x = p[x] = p[p[x]];
        return x;
    }
    void unite(std::uint32_t a, std::uint32_t b) { p[find(a)] = find(b); }
};
}  // namespace

std::size_t split_nonmanifold(Mesh& m) {
    const std::size_t nf = m.F.size(), nv = m.V.size();
    if (!nf) return 0;
    // half-edges keyed by the undirected edge (radix sort, no hashing)
    const int bits = bits_for(nv);
    std::vector<std::uint64_t> keys(3 * nf);
    std::vector<std::uint32_t> he(3 * nf);
    for (std::size_t f = 0; f < nf; ++f)
        for (int k = 0; k < 3; ++k) {
            std::uint64_t a = m.F[f][k], b = m.F[f][(k + 1) % 3];
            if (a > b) std::swap(a, b);
            keys[3 * f + k] = (a << bits) | b;
            he[3 * f + k] = std::uint32_t(3 * f + k);
        }
    radix_sort_pairs(keys, he, 2 * bits);
    // corner c = 3 f + k is the face f at vertex F[f][k]
    UnionFind uf(3 * nf);
    auto corner_at = [&](std::uint32_t f, std::uint32_t v) {
        for (int k = 0; k < 3; ++k)
            if (m.F[f][k] == v) return 3 * f + std::uint32_t(k);
        return 3 * f;
    };
    auto join = [&](std::uint32_t h1, std::uint32_t h2) {  // two half-edges forming one manifold edge
        const std::uint32_t f1 = h1 / 3, f2 = h2 / 3;
        const std::uint32_t a = m.F[f1][h1 % 3], b = m.F[f1][(h1 % 3 + 1) % 3];
        uf.unite(corner_at(f1, a), corner_at(f2, a));
        uf.unite(corner_at(f1, b), corner_at(f2, b));
    };
    std::size_t i = 0;
    std::vector<std::uint32_t> group;
    while (i < keys.size()) {
        std::size_t j = i;
        group.clear();
        while (j < keys.size() && keys[j] == keys[i]) group.push_back(he[j++]);
        i = j;
        if (group.size() == 2) {
            join(group[0], group[1]);
            continue;
        }
        if (group.size() < 2 || group.size() % 2) continue;  // open or odd: leave for the checker to report
        // sort the faces by angle around the edge u -> v
        const std::uint32_t h0 = group[0], f0 = h0 / 3;
        const std::uint32_t u = m.F[f0][h0 % 3], v = m.F[f0][(h0 % 3 + 1) % 3];
        const Vec3 e = normalized(m.V[v] - m.V[u]);
        Vec3 ax = cross(e, Vec3{1, 0, 0});
        if (norm(ax) < 0.5) ax = cross(e, Vec3{0, 1, 0});
        ax = normalized(ax);
        const Vec3 ay = cross(e, ax);
        struct Item {
            std::uint32_t h;
            double angle;
            Vec3 n;
        };
        std::vector<Item> items;
        for (auto h : group) {
            const auto& t = m.F[h / 3];
            const Vec3 w = m.V[t[(h % 3 + 2) % 3]];  // vertex opposite the edge
            const Vec3 r = w - m.V[u];
            const Vec3 rp = r - e * dot(r, e);
            const Vec3 n = cross(m.V[t[1]] - m.V[t[0]], m.V[t[2]] - m.V[t[0]]);
            items.push_back({h, std::atan2(dot(rp, ay), dot(rp, ax)), n});
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.angle < b.angle; });
        const std::size_t k = items.size();
        // the sector from item i to item i+1 is solid if it lies behind face i
        auto solid_after = [&](std::size_t idx) {
            const Item& a = items[idx];
            const Item& b = items[(idx + 1) % k];
            double mid = 0.5 * (a.angle + b.angle);
            if (b.angle < a.angle) mid += std::numbers::pi;  // wrap-around
            const Vec3 md = ax * std::cos(mid) + ay * std::sin(mid);
            return dot(md, a.n) < 0;
        };
        const std::size_t start = solid_after(0) ? 0 : 1;
        for (std::size_t q = 0; q < k; q += 2) join(items[(start + q) % k].h, items[(start + q + 1) % k].h);
    }
    // split vertices per corner group
    std::vector<std::int64_t> owner(nv, -1);  // root that keeps the original index
    std::vector<std::pair<std::uint32_t, std::uint32_t>> remap;  // (root, new vertex)
    std::size_t added = 0;
    std::vector<std::int64_t> root_vertex(3 * nf, -1);
    for (std::uint32_t c = 0; c < 3 * nf; ++c) {
        const std::uint32_t f = c / 3, kk = c % 3, v = m.F[f][kk];
        const std::uint32_t r = uf.find(c);
        if (root_vertex[r] < 0) {
            if (owner[v] < 0) {
                owner[v] = r;
                root_vertex[r] = v;
            } else {
                m.V.push_back(m.V[v]);
                root_vertex[r] = std::int64_t(m.V.size() - 1);
                ++added;
            }
        }
        m.F[f][kk] = std::uint32_t(root_vertex[r]);
    }
    return added;
}

}  // namespace anvil
