#include "anvil/validate.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <numeric>
#include <thread>
#include <unordered_map>

#include "anvil/bvh.hpp"
#include "anvil/sort.hpp"
#include "anvil/tolerance.hpp"

namespace anvil {

const char* issue_name(IssueKind k) {
    switch (k) {
        case IssueKind::InvalidIndex: return "invalid index";
        case IssueKind::Degenerate: return "degenerate triangle";
        case IssueKind::OpenEdge: return "open edge";
        case IssueKind::NonManifoldEdge: return "non-manifold edge";
        case IssueKind::InconsistentOrient: return "inconsistent orientation";
        case IssueKind::NonManifoldVertex: return "non-manifold vertex";
        case IssueKind::EulerMismatch: return "Euler characteristic";
        case IssueKind::NegativeVolume: return "inverted shell";
        case IssueKind::SelfIntersection: return "self-intersection";
        case IssueKind::Empty: return "empty mesh";
    }
    return "?";
}

std::string ValidationReport::summary() const {
    if (ok) {
        char buf[256];
        std::snprintf(buf, sizeof buf, "OK: %zu triangles, %zu shell(s), volume %.6g mm^3, area %.6g mm^2",
                      triangles, shells.size(), volume, area);
        return buf;
    }
    std::string s = "FAILED:";
    for (int k = 0; k < 10; ++k)
        if (counts[k]) s += " " + std::to_string(counts[k]) + "x " + issue_name(IssueKind(k)) + ";";
    return s;
}

namespace {

struct Ctx {
    const Mesh& m;
    ValidationReport& r;
    const ValidationOptions& opt;
    void add(IssueKind k, std::string msg, std::int64_t i = -1, Vec3 loc = {}, std::int64_t j = -1) {
        r.ok = false;
        auto& c = r.counts[int(k)];
        if (c++ < opt.max_issues_per_kind) r.issues.push_back({k, std::move(msg), i, j, loc});
    }
    Vec3 tri_center(std::size_t f) const {
        const auto& t = m.F[f];
        return (m.V[t[0]] + m.V[t[1]] + m.V[t[2]]) / 3.0;
    }
};

// ---------------------------------------------------------------- predicates
// Signed distances are compared against eps = kSelfIntersectRel * L * |n|
// (L = longest edge of the pair), i.e. a relative tolerance on the geometry.

struct V2 { double x, y; };
inline double orient2(V2 a, V2 b, V2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

int drop_axis(const Vec3& n) {
    double ax = std::abs(n.x), ay = std::abs(n.y), az = std::abs(n.z);
    return ax > ay ? (ax > az ? 0 : 2) : (ay > az ? 1 : 2);
}
V2 proj(const Vec3& p, int drop) {
    if (drop == 0) return {p.y, p.z};
    if (drop == 1) return {p.z, p.x};
    return {p.x, p.y};
}

// Proper crossing of 2D segments (interiors), with tolerance eps on orientations.
bool seg_cross2(V2 a, V2 b, V2 c, V2 d, double eps) {
    double o1 = orient2(a, b, c), o2 = orient2(a, b, d), o3 = orient2(c, d, a), o4 = orient2(c, d, b);
    return ((o1 > eps && o2 < -eps) || (o1 < -eps && o2 > eps)) &&
           ((o3 > eps && o4 < -eps) || (o3 < -eps && o4 > eps));
}
// Strictly inside triangle (abc) in 2D, orientation-independent.
bool inside2(V2 p, V2 a, V2 b, V2 c, double eps) {
    double s = orient2(a, b, c) > 0 ? 1.0 : -1.0;
    return s * orient2(a, b, p) > eps && s * orient2(b, c, p) > eps && s * orient2(c, a, p) > eps;
}

bool coplanar_overlap(const Vec3 P[3], const Vec3 Q[3], const Vec3& n, double eps2) {
    int d = drop_axis(n);
    V2 p[3] = {proj(P[0], d), proj(P[1], d), proj(P[2], d)};
    V2 q[3] = {proj(Q[0], d), proj(Q[1], d), proj(Q[2], d)};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (seg_cross2(p[i], p[(i + 1) % 3], q[j], q[(j + 1) % 3], eps2)) return true;
    for (int i = 0; i < 3; ++i) {
        if (inside2(p[i], q[0], q[1], q[2], eps2)) return true;
        if (inside2(q[i], p[0], p[1], p[2], eps2)) return true;
    }
    // identical triangles: centroids inside each other
    V2 cp{(p[0].x + p[1].x + p[2].x) / 3, (p[0].y + p[1].y + p[2].y) / 3};
    return inside2(cp, q[0], q[1], q[2], eps2);
}

// Intersection interval of triangle T (distances dist to the other plane) on the
// line direction D; returns false if T does not reach the plane.
bool plane_interval(const Vec3 T[3], const double dist[3], const Vec3& D, double& lo, double& hi) {
    lo = INFINITY;
    hi = -INFINITY;
    bool any = false;
    for (int i = 0; i < 3; ++i) {
        int j = (i + 1) % 3;
        if (dist[i] == 0.0) {
            double t = dot(D, T[i]);
            lo = std::min(lo, t), hi = std::max(hi, t), any = true;
        }
        if ((dist[i] < 0 && dist[j] > 0) || (dist[i] > 0 && dist[j] < 0)) {
            double s = dist[i] / (dist[i] - dist[j]);
            Vec3 x = T[i] + (T[j] - T[i]) * s;
            double t = dot(D, x);
            lo = std::min(lo, t), hi = std::max(hi, t), any = true;
        }
    }
    return any;
}

// Full triangle-triangle test for triangles that share no vertex.
bool tri_tri(const Vec3 P[3], const Vec3 Q[3], double L, double abs_tol) {
    Vec3 n1 = cross(P[1] - P[0], P[2] - P[0]);
    Vec3 n2 = cross(Q[1] - Q[0], Q[2] - Q[0]);
    const double r = std::max(tol::kSelfIntersectRel * L, abs_tol);  // distance tolerance (mm)
    // disjoint boxes (BVH leaves hold several triangles, so pairs arrive unfiltered)
    for (int a = 0; a < 3; ++a) {
        const double plo = std::min({P[0][a], P[1][a], P[2][a]}), phi = std::max({P[0][a], P[1][a], P[2][a]});
        const double qlo = std::min({Q[0][a], Q[1][a], Q[2][a]}), qhi = std::max({Q[0][a], Q[1][a], Q[2][a]});
        if (plo > qhi + r || qlo > phi + r) return false;
    }
    double e1 = r * norm(n1), e2 = r * norm(n2);
    double dq[3], dp[3];
    for (int i = 0; i < 3; ++i) {
        dq[i] = dot(n1, Q[i] - P[0]);
        if (std::abs(dq[i]) <= e1) dq[i] = 0.0;
        dp[i] = dot(n2, P[i] - Q[0]);
        if (std::abs(dp[i]) <= e2) dp[i] = 0.0;
    }
    auto same_side = [](const double d[3]) {
        return (d[0] > 0 && d[1] > 0 && d[2] > 0) || (d[0] < 0 && d[1] < 0 && d[2] < 0);
    };
    if (same_side(dq) || same_side(dp)) return false;
    // coplanar at this tolerance if either triangle lies in the other's plane
    if ((dq[0] == 0 && dq[1] == 0 && dq[2] == 0) || (dp[0] == 0 && dp[1] == 0 && dp[2] == 0))
        return coplanar_overlap(P, Q, n1, r * L);
    // The two triangles straddle each other's planes (else same_side returned).
    // Distances are only known to +-r, so points on the intersection line are
    // only known to r / sin(theta); the interval test along that line is
    // meaningful while this is small against the triangles (< 1 % of L). Else
    // the planes are nearly parallel at this tolerance: decide in the common plane.
    const double sin_angle = norm(cross(n1, n2)) / std::max(norm(n1) * norm(n2), 1e-300);
    if (r >= 0.01 * L * sin_angle) return coplanar_overlap(P, Q, n1, r * L);
    Vec3 D = normalized(cross(n1, n2));
    double a0, a1, b0, b1;
    if (!plane_interval(P, dp, D, a0, a1) || !plane_interval(Q, dq, D, b0, b1)) return false;
    double overlap = std::min(a1, b1) - std::max(a0, b0);
    return overlap > r;  // touching is not an intersection
}

// Segment (a,b) crosses the interior of triangle T (strict, tolerance-relative).
bool seg_tri(const Vec3& a, const Vec3& b, const Vec3 T[3]) {
    const double eps = tol::kSelfIntersectRel * 16;
    Vec3 d = b - a, e1 = T[1] - T[0], e2 = T[2] - T[0];
    Vec3 pv = cross(d, e2);
    double det = dot(e1, pv);
    if (std::abs(det) <= tol::kSelfIntersectRel * norm(d) * norm(e1) * norm(e2)) return false;
    double id = 1.0 / det;
    Vec3 tv = a - T[0];
    double u = dot(tv, pv) * id;
    Vec3 qv = cross(tv, e1);
    double v = dot(d, qv) * id;
    double t = dot(e2, qv) * id;
    return u > eps && v > eps && u + v < 1.0 - eps && t > eps && t < 1.0 - eps;
}

bool shared_vertex_intersects(const Vec3 P[3], int ip, const Vec3 Q[3], int iq, double L, double abs_tol) {
    // P[ip] == Q[iq] is the shared vertex v.
    const Vec3& p1 = P[(ip + 1) % 3]; const Vec3& p2 = P[(ip + 2) % 3];
    const Vec3& q1 = Q[(iq + 1) % 3]; const Vec3& q2 = Q[(iq + 2) % 3];
    Vec3 n1 = cross(P[1] - P[0], P[2] - P[0]);
    const double r = std::max(tol::kSelfIntersectRel * L, abs_tol);
    double e1 = r * norm(n1);
    bool coplanar = std::abs(dot(n1, q1 - P[ip])) <= e1 && std::abs(dot(n1, q2 - P[ip])) <= e1;
    if (!coplanar) return seg_tri(p1, p2, Q) || seg_tri(q1, q2, P);
    // Coplanar: wedges at v overlap, or far edges cross.
    int d = drop_axis(n1);
    V2 v = proj(P[ip], d), a1 = proj(p1, d), a2 = proj(p2, d), b1 = proj(q1, d), b2 = proj(q2, d);
    const double eps2 = r * L;
    auto in_wedge = [&](V2 x, V2 w1, V2 w2) {  // x strictly inside the wedge (v; w1, w2)
        double s = orient2(v, w1, w2) > 0 ? 1.0 : -1.0;
        return s * orient2(v, w1, x) > eps2 && s * orient2(v, x, w2) > eps2;
    };
    V2 mb{(b1.x + b2.x) / 2, (b1.y + b2.y) / 2}, ma{(a1.x + a2.x) / 2, (a1.y + a2.y) / 2};
    if (in_wedge(b1, a1, a2) || in_wedge(b2, a1, a2) || in_wedge(mb, a1, a2) ||
        in_wedge(a1, b1, b2) || in_wedge(a2, b1, b2) || in_wedge(ma, b1, b2))
        return true;
    return seg_cross2(a1, a2, b1, b2, eps2);
}

bool shared_edge_fold(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, double L, double abs_tol) {
    // Triangles (a,b,c) and (b,a,d) share edge ab. They overlap iff coplanar and
    // c, d lie on the same side of ab.
    Vec3 n1 = cross(b - a, c - a);
    if (std::abs(dot(n1, d - a)) > std::max(tol::kSelfIntersectRel * L, abs_tol) * norm(n1)) return false;
    Vec3 n2 = cross(b - a, d - a);
    return dot(n1, n2) > 0;
}

void check_self_intersections(Ctx& ctx, double abs_tol) {
    const Mesh& m = ctx.m;
    if (m.F.empty()) return;
    Bvh bvh(m);
    const std::size_t n = m.F.size();
    std::vector<std::pair<std::uint32_t, std::uint32_t>> hits;
    std::mutex mu;
    unsigned nt = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    auto work = [&](std::size_t begin, std::size_t end) {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> local;
        for (std::size_t i = begin; i < end; ++i) {
            const auto& ti = m.F[i];
            Vec3 P[3] = {m.V[ti[0]], m.V[ti[1]], m.V[ti[2]]};
            Box3 bi;
            for (auto& p : P) bi.add(p);
            bvh.query(bi, [&](std::uint32_t j) {
                if (j <= i) return;
                const auto& tj = m.F[j];
                Vec3 Q[3] = {m.V[tj[0]], m.V[tj[1]], m.V[tj[2]]};
                double L = 0;
                for (int k = 0; k < 3; ++k) {
                    L = std::max(L, norm(P[(k + 1) % 3] - P[k]));
                    L = std::max(L, norm(Q[(k + 1) % 3] - Q[k]));
                }
                int si[3], sj[3], s = 0;
                for (int a = 0; a < 3; ++a)
                    for (int b = 0; b < 3; ++b)
                        if (ti[a] == tj[b]) si[s] = a, sj[s] = b, ++s;
                bool x = false;
                if (s == 0) x = tri_tri(P, Q, L, abs_tol);
                else if (s == 1) x = shared_vertex_intersects(P, si[0], Q, sj[0], L, abs_tol);
                else if (s == 2) {
                    int ci = 3 - si[0] - si[1], cj = 3 - sj[0] - sj[1];
                    x = shared_edge_fold(m.V[ti[si[0]]], m.V[ti[si[1]]], P[ci], Q[cj], L, abs_tol);
                } else x = true;  // duplicate triangle
                if (x) local.emplace_back(std::uint32_t(i), j);
            });
        }
        std::lock_guard lk(mu);
        hits.insert(hits.end(), local.begin(), local.end());
    };
    std::vector<std::thread> pool;
    std::size_t chunk = (n + nt - 1) / nt;
    for (unsigned t = 0; t < nt; ++t) {
        std::size_t b = t * chunk, e = std::min(n, b + chunk);
        if (b < e) pool.emplace_back(work, b, e);
    }
    for (auto& t : pool) t.join();
    std::sort(hits.begin(), hits.end());
    for (auto [i, j] : hits) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "triangles #%u and #%u intersect", i, j);
        ctx.add(IssueKind::SelfIntersection, buf, i, (ctx.tri_center(i) + ctx.tri_center(j)) * 0.5, j);
    }
}

std::uint32_t find(std::vector<std::uint32_t>& p, std::uint32_t x) {
    while (p[x] != x) x = p[x] = p[p[x]];
    return x;
}

}  // namespace

bool relax_folds(Mesh& m, double abs_tol, int rounds) {
    // 1-ring adjacency once (topology does not change)
    std::vector<std::vector<std::uint32_t>> ring(m.V.size());
    for (const auto& t : m.F)
        for (int k = 0; k < 3; ++k) {
            ring[t[k]].push_back(t[(k + 1) % 3]);
            ring[t[k]].push_back(t[(k + 2) % 3]);
        }
    for (int r = 0; r < rounds; ++r) {
        ValidationOptions opt;
        opt.abs_tol_mm = abs_tol;
        opt.max_issues_per_kind = std::size_t(1) << 20;
        const ValidationReport rep = validate(m, opt);
        std::vector<std::uint32_t> verts;
        for (const auto& is : rep.issues)
            if (is.kind == IssueKind::SelfIntersection)
                for (auto t : {is.index, is.index2})
                    if (t >= 0)
                        for (int k = 0; k < 3; ++k) verts.push_back(m.F[std::size_t(t)][k]);
        if (verts.empty()) return true;
        std::sort(verts.begin(), verts.end());
        verts.erase(std::unique(verts.begin(), verts.end()), verts.end());
        // include the 1-ring: folds are relaxed with their neighbourhood
        std::vector<std::uint32_t> region = verts;
        for (auto v : verts) region.insert(region.end(), ring[v].begin(), ring[v].end());
        std::sort(region.begin(), region.end());
        region.erase(std::unique(region.begin(), region.end()), region.end());
        for (int it = 0; it < 3; ++it) {
            std::vector<Vec3> np(region.size());
            for (std::size_t i = 0; i < region.size(); ++i) {
                Vec3 s{};
                for (auto u : ring[region[i]]) s += m.V[u];
                np[i] = ring[region[i]].empty() ? m.V[region[i]] : s / double(ring[region[i]].size());
            }
            for (std::size_t i = 0; i < region.size(); ++i) m.V[region[i]] = np[i];
        }
    }
    ValidationOptions opt;
    opt.abs_tol_mm = abs_tol;
    return validate(m, opt).counts[int(IssueKind::SelfIntersection)] == 0;
}

double float_tolerance(const Mesh& m) {
    double mx = 0;
    for (const auto& p : m.V) mx = std::max({mx, std::abs(p.x), std::abs(p.y), std::abs(p.z)});
    return tol::kExportSliverUlps * std::ldexp(std::max(mx, 1.0), -23);  // float32: 23 fraction bits
}

ValidationReport validate(const Mesh& m, const ValidationOptions& opt) {
    auto t0 = std::chrono::steady_clock::now();
    ValidationReport r;
    Ctx ctx{m, r, opt};
    r.triangles = m.F.size();
    if (m.F.empty()) {
        ctx.add(IssueKind::Empty, "mesh has no triangles");
        return r;
    }
    const std::uint64_t nv = m.V.size();
    for (std::size_t f = 0; f < m.F.size(); ++f)
        for (auto v : m.F[f])
            if (v >= nv) {
                ctx.add(IssueKind::InvalidIndex, "triangle #" + std::to_string(f) + " references vertex " +
                                                     std::to_string(v) + " >= " + std::to_string(nv),
                        std::int64_t(f));
                return r;
            }

    // 1. degenerate triangles (scale-invariant threshold)
    const double diag = m.bounds().diagonal();
    const double thr2 = 2.0 * tol::kDegenerateAreaRel * diag * diag;  // threshold on |cross| = 2*area
    for (std::size_t f = 0; f < m.F.size(); ++f) {
        const auto& t = m.F[f];
        double a2 = norm(cross(m.V[t[1]] - m.V[t[0]], m.V[t[2]] - m.V[t[0]]));
        r.area += a2 / 2;
        if (a2 <= thr2) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "triangle #%zu is degenerate (area %.3e mm^2 <= %.3e mm^2)", f,
                          a2 / 2, thr2 / 2);
            ctx.add(IssueKind::Degenerate, buf, std::int64_t(f), ctx.tri_center(f));
        }
    }

    // 2. half-edge pairing. h = 3f+k is the directed edge F[f][k] -> F[f][k+1].
    // All half-edges are radix-sorted by their undirected key; a manifold,
    // consistently oriented edge is a run of exactly two opposite half-edges.
    const std::size_t nh = 3 * m.F.size();
    auto tail = [&](std::size_t h) { return m.F[h / 3][h % 3]; };
    auto head = [&](std::size_t h) { return m.F[h / 3][(h % 3 + 1) % 3]; };
    std::vector<std::uint64_t> keys(nh);
    std::vector<std::uint32_t> hes(nh);
    for (std::size_t h = 0; h < nh; ++h) {
        std::uint64_t a = tail(h), b = head(h);
        keys[h] = std::min(a, b) * nv + std::max(a, b);
        hes[h] = std::uint32_t(h);
    }
    radix_sort_pairs(keys, hes, bits_for(nv * nv));
    std::vector<std::uint32_t> twin(nh, UINT32_MAX);
    bool edge_ok = true;
    std::size_t E = 0;
    std::vector<std::uint64_t> edge_first;  // one half-edge per undirected edge (for shell edge counts)
    edge_first.reserve(nh / 2);
    for (std::size_t i = 0; i < nh;) {
        std::size_t j = i + 1;
        while (j < nh && keys[j] == keys[i]) ++j;
        const std::size_t c = j - i;
        ++E;
        edge_first.push_back(hes[i]);
        const auto a = keys[i] / nv, b = keys[i] % nv;
        if (c == 2) {
            std::uint32_t h1 = hes[i], h2 = hes[i + 1];
            if (tail(h1) == tail(h2)) {
                edge_ok = false;
                ctx.add(IssueKind::InconsistentOrient,
                        "half-edge (" + std::to_string(tail(h1)) + "->" + std::to_string(head(h1)) +
                            ") used twice: flipped triangle (triangles #" + std::to_string(h1 / 3) + ", #" +
                            std::to_string(h2 / 3) + ")",
                        std::int64_t(h2 / 3), m.V[tail(h1)]);
            } else {
                twin[h1] = h2;
                twin[h2] = h1;
            }
        } else {
            edge_ok = false;
            bool open = c == 1;
            ctx.add(open ? IssueKind::OpenEdge : IssueKind::NonManifoldEdge,
                    std::string(open ? "open (boundary) edge" : "non-manifold edge") + " (" + std::to_string(a) + "-" +
                        std::to_string(b) + ") has " + std::to_string(c) + " incident triangle(s), expected 2",
                    std::int64_t(a), (m.V[a] + m.V[b]) * 0.5);
        }
        i = j;
    }
    keys.clear();
    keys.shrink_to_fit();
    hes.clear();
    hes.shrink_to_fit();
    if (edge_ok && 3 * m.F.size() != 2 * E)
        ctx.add(IssueKind::EulerMismatch, "3F != 2E (" + std::to_string(3 * m.F.size()) + " != " +
                                              std::to_string(2 * E) + ")");

    // 3. vertex manifoldness: count triangle fans per vertex via rot(h) = twin(next(h)).
    if (edge_ok) {
        std::vector<std::uint8_t> seen(nh, 0);
        std::vector<std::uint32_t> fans(nv, 0);
        for (std::size_t h0 = 0; h0 < nh; ++h0) {
            if (seen[h0]) continue;
            std::size_t h = h0;
            do {  // h is an incoming half-edge of head(h0)
                seen[h] = 1;
                std::size_t nxt = (h / 3) * 3 + (h % 3 + 1) % 3;
                h = twin[nxt];
            } while (h != h0);
            ++fans[head(h0)];
        }
        for (std::uint64_t v = 0; v < nv; ++v)
            if (fans[v] > 1)
                ctx.add(IssueKind::NonManifoldVertex,
                        "non-manifold vertex #" + std::to_string(v) + " (" + std::to_string(fans[v]) +
                            " triangle fans meet here)",
                        std::int64_t(v), m.V[v]);
    }

    // 4./5. shells: Euler characteristic and signed volume
    std::vector<std::uint32_t> parent(nv);
    std::iota(parent.begin(), parent.end(), 0u);
    for (const auto& t : m.F) {
        auto a = find(parent, t[0]);
        for (int k = 1; k < 3; ++k) {
            auto b = find(parent, t[k]);
            if (a != b) parent[std::max(a, b)] = std::min(a, b), a = std::min(a, b);
        }
    }
    std::unordered_map<std::uint32_t, std::size_t> shell_of;
    std::vector<ShellInfo> shells;
    std::vector<std::uint8_t> vused(nv, 0);
    for (const auto& t : m.F) {
        auto root = find(parent, t[0]);
        auto [it, ins] = shell_of.try_emplace(root, shells.size());
        if (ins) shells.push_back({});
        auto& s = shells[it->second];
        ++s.faces;
        s.volume += dot(m.V[t[0]], cross(m.V[t[1]], m.V[t[2]])) / 6.0;
        for (auto v : t)
            if (!vused[v]) vused[v] = 1, ++s.vertices;
    }
    for (std::uint64_t h : edge_first) ++shells[shell_of[find(parent, tail(h))]].edges;
    for (std::size_t i = 0; i < shells.size(); ++i) {
        auto& s = shells[i];
        s.euler = long(s.vertices) - long(s.edges) + long(s.faces);
        long two_g = 2 - s.euler;
        s.genus = two_g / 2;
        r.volume += s.volume;
        if (edge_ok && (two_g < 0 || two_g % 2))
            ctx.add(IssueKind::EulerMismatch, "shell " + std::to_string(i) + ": V - E + F = " +
                                                  std::to_string(s.euler) + " is not 2(1 - g) with g >= 0");
        if (!(s.volume > 0))
            ctx.add(IssueKind::NegativeVolume, "shell " + std::to_string(i) + ": signed volume " +
                                                   std::to_string(s.volume) + " mm^3 <= 0 (normals point inward)");
    }
    r.shells = std::move(shells);

    if (opt.self_intersections && edge_ok) check_self_intersections(ctx, opt.abs_tol_mm);
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

}  // namespace anvil
