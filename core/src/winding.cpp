// Fast generalized winding numbers (see winding.hpp for the formulas).
#include "anvil/winding.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_map>

namespace anvil {

double solid_angle(const Vec3& a, const Vec3& b, const Vec3& c) {
    const double la = norm(a), lb = norm(b), lc = norm(c);
    const double num = dot(a, cross(b, c));
    const double den = la * lb * lc + dot(a, b) * lc + dot(b, c) * la + dot(c, a) * lb;
    return 2.0 * std::atan2(num, den);
}

std::size_t cap_boundaries(Mesh& m) {
    // net orientation count per undirected edge
    auto key = [](std::uint32_t a, std::uint32_t b) { return (std::uint64_t(a) << 32) | b; };
    std::unordered_map<std::uint64_t, int> net;
    net.reserve(m.F.size() * 3);
    for (const auto& f : m.F)
        for (int k = 0; k < 3; ++k) {
            std::uint32_t a = f[std::size_t(k)], b = f[std::size_t((k + 1) % 3)];
            if (a < b) ++net[key(a, b)];
            else --net[key(b, a)];
        }
    // boundary edges a->b (with multiplicity), union-find over their vertices
    std::vector<std::array<std::uint32_t, 2>> bnd;
    for (const auto& [k, c] : net) {
        const auto a = std::uint32_t(k >> 32), b = std::uint32_t(k & 0xFFFFFFFFu);
        for (int i = 0; i < std::abs(c); ++i) bnd.push_back(c > 0 ? std::array{a, b} : std::array{b, a});
    }
    if (bnd.empty()) return 0;
    std::sort(bnd.begin(), bnd.end());  // deterministic output order
    std::unordered_map<std::uint32_t, std::uint32_t> uf;
    auto find = [&](std::uint32_t x) {
        auto it = uf.try_emplace(x, x).first;
        while (it->second != x) {
            auto& px = it->second;
            auto pit = uf.find(px);
            px = pit->second;  // path halving
            x = px;
            it = uf.find(x);
        }
        return x;
    };
    for (const auto& e : bnd) {
        const auto ra = find(e[0]), rb = find(e[1]);
        if (ra != rb) uf[std::max(ra, rb)] = std::min(ra, rb);
    }
    // length-weighted centroid of each component
    std::unordered_map<std::uint32_t, std::pair<Vec3, double>> acc;
    for (const auto& e : bnd) {
        const Vec3 &a = m.V[e[0]], &b = m.V[e[1]];
        const double l = norm(b - a);
        auto& [s, w] = acc[find(e[0])];
        s += (a + b) * (0.5 * l);
        w += l;
    }
    std::unordered_map<std::uint32_t, std::uint32_t> apex;
    std::vector<std::uint32_t> roots;
    for (const auto& [r, _] : acc) roots.push_back(r);
    std::sort(roots.begin(), roots.end());
    for (auto r : roots) {
        const auto& [s, w] = acc[r];
        apex[r] = std::uint32_t(m.V.size());
        m.V.push_back(w > 0 ? s / w : m.V[r]);
    }
    for (const auto& e : bnd) m.F.push_back({e[1], e[0], apex[find(e[0])]});
    return bnd.size();
}

WindingNumber::WindingNumber(const Mesh& m, double beta) : m_(m), beta2_(beta * beta) {
    order_.resize(m.F.size());
    std::vector<Vec3> cent(m.F.size());
    for (std::uint32_t t = 0; t < m.F.size(); ++t) {
        order_[t] = t;
        cent[t] = (m.V[m.F[t][0]] + m.V[m.F[t][1]] + m.V[m.F[t][2]]) / 3.0;
    }
    nodes_.reserve(2 * m.F.size() / 4 + 1);
    if (!m.F.empty()) build(0, std::uint32_t(m.F.size()), cent);
}

std::uint32_t WindingNumber::build(std::uint32_t first, std::uint32_t count, std::vector<Vec3>& cent) {
    constexpr std::uint32_t kLeaf = 8;  // triangles per leaf (exact evaluation)
    const auto id = std::uint32_t(nodes_.size());
    nodes_.emplace_back();
    Node nd;
    nd.first = first;
    double area = 0;
    Vec3 cs{0, 0, 0}, nsum{0, 0, 0};
    Box3 cb;
    for (std::uint32_t i = first; i < first + count; ++i) {
        const auto& f = m_.F[order_[i]];
        const Vec3 &a = m_.V[f[0]], &b = m_.V[f[1]], &c = m_.V[f[2]];
        const Vec3 an = cross(b - a, c - a) * 0.5;  // A_t n_t
        const double A = norm(an);
        nsum += an;
        cs += cent[order_[i]] * A;
        area += A;
        cb.add(cent[order_[i]]);
    }
    nd.n = nsum;
    nd.c = area > 0 ? cs / area : cb.center();
    for (std::uint32_t i = first; i < first + count; ++i)
        for (auto v : m_.F[order_[i]]) {
            const Vec3 d = m_.V[v] - nd.c;
            nd.r2 = std::max(nd.r2, dot(d, d));
        }
    if (count <= kLeaf) {
        nd.count = count;
        nodes_[id] = nd;
        return id;
    }
    // median split along the longest axis of the centroid box
    const Vec3 ext = cb.hi - cb.lo;
    const int ax = ext.x >= ext.y && ext.x >= ext.z ? 0 : (ext.y >= ext.z ? 1 : 2);
    const std::uint32_t mid = first + count / 2;
    std::nth_element(order_.begin() + first, order_.begin() + mid, order_.begin() + first + count,
                     [&](std::uint32_t a, std::uint32_t b) { return cent[a][ax] < cent[b][ax]; });
    nd.count = 0;
    nd.left = build(first, mid - first, cent);
    nd.right = build(mid, first + count - mid, cent);
    nodes_[id] = nd;
    return id;
}

double WindingNumber::operator()(const Vec3& p) const {
    if (nodes_.empty()) return 0.0;
    double sum = 0;  // in steradians
    std::uint32_t stack[128];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const Node& nd = nodes_[stack[--sp]];
        const Vec3 d = nd.c - p;
        const double d2 = dot(d, d);
        if (d2 > beta2_ * nd.r2) {
            sum += dot(nd.n, d) / (d2 * std::sqrt(d2));  // dipole (far field)
            continue;
        }
        if (nd.count > 0) {
            for (std::uint32_t i = nd.first; i < nd.first + nd.count; ++i) {
                const auto& f = m_.F[order_[i]];
                sum += solid_angle(m_.V[f[0]] - p, m_.V[f[1]] - p, m_.V[f[2]] - p);
            }
            continue;
        }
        // median splits: depth <= log2(n / 8) + 1, so at most ~2 * 32 entries
        stack[sp++] = nd.left;
        stack[sp++] = nd.right;
    }
    return sum / (4.0 * std::numbers::pi);
}

double WindingNumber::exact(const Vec3& p) const {
    double sum = 0;
    for (const auto& f : m_.F) sum += solid_angle(m_.V[f[0]] - p, m_.V[f[1]] - p, m_.V[f[2]] - p);
    return sum / (4.0 * std::numbers::pi);
}

}  // namespace anvil
