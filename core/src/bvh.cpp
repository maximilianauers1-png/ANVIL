#include "anvil/bvh.hpp"

#include <algorithm>
#include <numeric>

namespace anvil {

namespace {
constexpr std::uint32_t kLeafSize = 4;
}

Bvh::Bvh(const Mesh& m) : mesh_(&m) {
    const auto n = static_cast<std::uint32_t>(m.F.size());
    if (n == 0) return;
    std::vector<Vec3> c(n);
    std::vector<Box3> b(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto& t = m.F[i];
        b[i].add(m.V[t[0]]);
        b[i].add(m.V[t[1]]);
        b[i].add(m.V[t[2]]);
        c[i] = b[i].center();
    }
    order_.resize(n);
    std::iota(order_.begin(), order_.end(), 0u);
    nodes_.reserve(2 * n / kLeafSize + 1);
    build(0, n, c, b);
}

std::uint32_t Bvh::build(std::uint32_t lo, std::uint32_t hi, std::vector<Vec3>& c, std::vector<Box3>& b) {
    const auto idx = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back({});
    Box3 box, cbox;
    for (std::uint32_t i = lo; i < hi; ++i) {
        box.add(b[order_[i]]);
        cbox.add(c[order_[i]]);
    }
    nodes_[idx].box = box;
    if (hi - lo <= kLeafSize) {
        nodes_[idx].first = lo;
        nodes_[idx].count = hi - lo;
        return idx;
    }
    Vec3 ext = cbox.hi - cbox.lo;
    int axis = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
    std::uint32_t mid = lo + (hi - lo) / 2;
    std::nth_element(order_.begin() + lo, order_.begin() + mid, order_.begin() + hi,
                     [&](std::uint32_t a, std::uint32_t bb) {
                         double ca = c[a][axis], cb = c[bb][axis];
                         return ca < cb || (ca == cb && a < bb);  // deterministic tie-break
                     });
    std::uint32_t left = build(lo, mid, c, b);
    std::uint32_t right = build(mid, hi, c, b);
    nodes_[idx].first = left;
    nodes_[idx].right = right;
    nodes_[idx].count = 0;
    return idx;
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
