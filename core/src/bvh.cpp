#include "anvil/bvh.hpp"

#include "anvil/sort.hpp"

#include <algorithm>
#include <numeric>

namespace anvil {

namespace {
constexpr std::uint32_t kLeafSize = 4;
}

namespace {
// Spreads the lower 10 bits of x to every third bit (Morton code helper).
std::uint32_t spread3(std::uint32_t x) {
    x &= 0x3ff;
    x = (x | (x << 16)) & 0x030000ff;
    x = (x | (x << 8)) & 0x0300f00f;
    x = (x | (x << 4)) & 0x030c30c3;
    x = (x | (x << 2)) & 0x09249249;
    return x;
}
}  // namespace

// Linear BVH (Karras 2012 style, top-down on sorted Morton codes): centroids are
// quantized to a 1024^3 grid, radix-sorted by their 30-bit Morton code, and
// split at the highest differing code bit. Build is O(n) after the sort; boxes
// are filled bottom-up.
Bvh::Bvh(const Mesh& m) : mesh_(&m) {
    const auto n = static_cast<std::uint32_t>(m.F.size());
    if (n == 0) return;
    std::vector<Vec3> c(n);
    Box3 cb;
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto& t = m.F[i];
        c[i] = (m.V[t[0]] + m.V[t[1]] + m.V[t[2]]) / 3.0;
        cb.add(c[i]);
    }
    Vec3 ext = cb.hi - cb.lo;
    std::vector<std::uint64_t> codes(n);
    order_.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        auto q = [&](int a) {
            double e = ext[a] > 0 ? (c[i][a] - cb.lo[a]) / ext[a] : 0.0;
            return std::uint32_t(std::clamp(e * 1023.0, 0.0, 1023.0));
        };
        codes[i] = (spread3(q(0)) << 2) | (spread3(q(1)) << 1) | spread3(q(2));
        order_[i] = i;
    }
    radix_sort_pairs(codes, order_, 30);
    nodes_.reserve(2 * (n / kLeafSize + 1));
    // iterative top-down split (explicit stack, no recursion depth issues)
    struct Job { std::uint32_t node, lo, hi; };
    std::vector<Job> stack;
    nodes_.push_back({});
    stack.push_back({0, 0, n});
    while (!stack.empty()) {
        Job j = stack.back();
        stack.pop_back();
        if (j.hi - j.lo <= kLeafSize) {
            nodes_[j.node].first = j.lo;
            nodes_[j.node].count = j.hi - j.lo;
            continue;
        }
        std::uint64_t a = codes[j.lo], b = codes[j.hi - 1];
        std::uint32_t mid;
        if (a == b) {
            mid = j.lo + (j.hi - j.lo) / 2;  // identical codes: median split
        } else {
            int bit = 63;
            while (!(((a ^ b) >> bit) & 1)) --bit;
            // first index whose code has `bit` set (codes are sorted)
            std::uint32_t lo = j.lo, hi = j.hi - 1;
            while (lo < hi) {
                std::uint32_t md = lo + (hi - lo) / 2;
                if ((codes[md] >> bit) & 1) hi = md;
                else lo = md + 1;
            }
            mid = lo;
        }
        auto left = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back({});
        auto right = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back({});
        nodes_[j.node].first = left;
        nodes_[j.node].right = right;
        nodes_[j.node].count = 0;
        stack.push_back({right, mid, j.hi});
        stack.push_back({left, j.lo, mid});
    }
    // bottom-up boxes: children always have larger indices than their parent
    for (std::size_t k = nodes_.size(); k-- > 0;) {
        Node& nd = nodes_[k];
        Box3 bx;
        if (nd.count) {
            for (std::uint32_t i = nd.first; i < nd.first + nd.count; ++i) {
                const auto& t = m.F[order_[i]];
                bx.add(m.V[t[0]]), bx.add(m.V[t[1]]), bx.add(m.V[t[2]]);
            }
        } else {
            bx = nodes_[nd.first].box;
            bx.add(nodes_[nd.right].box);
        }
        nd.box = bx;
    }
}

namespace {
bool ray_box(const Box3& b, const Vec3& o, const Vec3& inv, double t0, double t1) {
    for (int a = 0; a < 3; ++a) {
        double tn = (b.lo[a] - o[a]) * inv[a];
        double tf = (b.hi[a] - o[a]) * inv[a];
        if (tn > tf) std::swap(tn, tf);
        t0 = tn > t0 ? tn : t0;
        t1 = tf < t1 ? tf : t1;
        if (t0 > t1) return false;
    }
    return true;
}
}  // namespace

std::optional<RayHit> Bvh::raycast(const Vec3& o, const Vec3& d, double t_min, double t_max) const {
    if (nodes_.empty()) return std::nullopt;
    Vec3 inv{1.0 / d.x, 1.0 / d.y, 1.0 / d.z};
    std::optional<RayHit> best;
    double best_t = t_max;
    std::uint32_t stack[128];
    int sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const Node& nd = nodes_[stack[--sp]];
        if (!ray_box(nd.box, o, inv, t_min, best_t)) continue;
        if (nd.count) {
            for (std::uint32_t k = nd.first; k < nd.first + nd.count; ++k) {
                const auto& t = mesh_->F[order_[k]];
                const Vec3 &p0 = mesh_->V[t[0]], &p1 = mesh_->V[t[1]], &p2 = mesh_->V[t[2]];
                Vec3 e1 = p1 - p0, e2 = p2 - p0;
                Vec3 pv = cross(d, e2);
                double det = dot(e1, pv);
                if (det == 0.0) continue;
                double id = 1.0 / det;
                Vec3 tv = o - p0;
                double u = dot(tv, pv) * id;
                if (u < 0.0 || u > 1.0) continue;
                Vec3 qv = cross(tv, e1);
                double v = dot(d, qv) * id;
                if (v < 0.0 || u + v > 1.0) continue;
                double tt = dot(e2, qv) * id;
                if (tt > t_min && tt < best_t) {
                    best_t = tt;
                    best = RayHit{order_[k], tt, o + d * tt, normalized(cross(e1, e2))};
                }
            }
        } else if (sp + 2 <= 128) {
            stack[sp++] = nd.first;
            stack[sp++] = nd.right;
        }
    }
    return best;
}

void Bvh::query(const Box3& box, const std::function<void(std::uint32_t)>& f) const {
    if (nodes_.empty()) return;
    std::uint32_t stack[128];
    int sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const Node& nd = nodes_[stack[--sp]];
        if (!nd.box.overlaps(box)) continue;
        if (nd.count) {
            for (std::uint32_t k = nd.first; k < nd.first + nd.count; ++k) f(order_[k]);
        } else if (sp + 2 <= 128) {
            stack[sp++] = nd.first;
            stack[sp++] = nd.right;
        }
    }
}

}  // namespace anvil
