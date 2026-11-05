#include "anvil/sdf.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "anvil/bvh.hpp"

namespace anvil::sdf {

// ------------------------------------------------------------------ binary16
std::uint16_t float_to_half(float f) {
    std::uint32_t x;
    std::memcpy(&x, &f, 4);
    const std::uint32_t sign = (x >> 16) & 0x8000u;
    const std::int32_t e8 = std::int32_t((x >> 23) & 0xffu);
    std::uint32_t mant = x & 0x7fffffu;
    if (e8 == 0xff) return std::uint16_t(sign | 0x7c00u | (mant ? 0x200u : 0u));  // inf / nan
    const std::int32_t e = e8 - 127 + 15;
    if (e >= 31) return std::uint16_t(sign | 0x7c00u);                         // overflow -> inf
    if (e <= 0) {                                                             // subnormal / zero
        if (e < -10) return std::uint16_t(sign);
        mant |= 0x800000u;
        const int shift = 14 - e;
        std::uint32_t h = mant >> shift;
        const std::uint32_t rem = mant & ((1u << shift) - 1u), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (h & 1u))) ++h;
        return std::uint16_t(sign | h);
    }
    std::uint32_t h = (std::uint32_t(e) << 10) | (mant >> 13);
    const std::uint32_t rem = mant & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) ++h;  // a carry into the exponent is correct
    return std::uint16_t(sign | h);
}

namespace {
float half_to_float_slow(std::uint16_t h) {
    const std::uint32_t sign = std::uint32_t(h & 0x8000u) << 16;
    std::uint32_t e = (h >> 10) & 0x1fu, m = h & 0x3ffu, x;
    if (e == 0) {
        if (m == 0) x = sign;
        else {  // subnormal: normalise
            e = 1;
            while (!(m & 0x400u)) m <<= 1, --e;
            m &= 0x3ffu;
            x = sign | ((e + 127 - 15) << 23) | (m << 13);
        }
    } else if (e == 31) {
        x = sign | 0x7f800000u | (m << 13);
    } else {
        x = sign | ((e + 127 - 15) << 23) | (m << 13);
    }
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}
struct HalfTable {
    std::vector<float> t;
    HalfTable() : t(65536) {
        for (std::uint32_t i = 0; i < 65536; ++i) t[i] = half_to_float_slow(std::uint16_t(i));
    }
};
const HalfTable& half_table() {
    static const HalfTable tab;
    return tab;
}
std::atomic<std::uint64_t> g_stamp{1};

template <class F>
void parallel_for(std::size_t n, F&& f) {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t nt = std::min<std::size_t>(hw, (n + 31) / 32);
    if (nt <= 1) {
        for (std::size_t i = 0; i < n; ++i) f(i);
        return;
    }
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> th;
    for (std::size_t t = 0; t < nt; ++t)
        th.emplace_back([&] {
            for (std::size_t i; (i = next.fetch_add(16)) < n;)
                for (std::size_t k = i; k < std::min(n, i + 16); ++k) f(k);
        });
    for (auto& t : th) t.join();
}

// Wendland C2 kernel (Wendland 1995): compact support, C2 at q = 1.
inline double wendland(double q) {
    if (q >= 1.0) return 0.0;
    const double a = 1.0 - q;
    return a * a * a * a * (1.0 + 4.0 * q);
}
}  // namespace

float half_to_float(std::uint16_t h) { return half_table().t[h]; }

// ------------------------------------------------------------------ Grid
float Grid::at(int i, int j, int k) const {
    const IVec3 b{floor_div(i, kBrick), floor_div(j, kBrick), floor_div(k, kBrick)};
    auto it = bricks_.find(b);
    if (it == bricks_.end()) return interior_.count(b) ? -kBand : kBand;
    return half_table().t[it->second->v[Brick::index(i - b.x * kBrick, j - b.y * kBrick, k - b.z * kBrick)]];
}

double Grid::sample(const Vec3& p) const {
    const double x = p.x / h_, y = p.y / h_, z = p.z / h_;
    const int i = int(std::floor(x)), j = int(std::floor(y)), k = int(std::floor(z));
    const double u = x - i, v = y - j, w = z - k;
    double c[2][2][2];
    for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) c[dz][dy][dx] = at(i + dx, j + dy, k + dz);
    auto lerp = [](double a, double b, double t) { return a + (b - a) * t; };
    double f = lerp(lerp(lerp(c[0][0][0], c[0][0][1], u), lerp(c[0][1][0], c[0][1][1], u), v),
                    lerp(lerp(c[1][0][0], c[1][0][1], u), lerp(c[1][1][0], c[1][1][1], u), v), w);
    return f * h_;
}

Vec3 Grid::gradient(const Vec3& p) const {
    const double e = h_ / 2;
    return Vec3{sample(p + Vec3{e, 0, 0}) - sample(p - Vec3{e, 0, 0}), sample(p + Vec3{0, e, 0}) - sample(p - Vec3{0, e, 0}),
                sample(p + Vec3{0, 0, e}) - sample(p - Vec3{0, 0, e})} /
           (2 * e);
}

Brick& Grid::mutable_brick(const IVec3& b) {
    auto it = bricks_.find(b);
    if (it == bricks_.end()) {
        auto nb = std::make_shared<Brick>();
        const std::uint16_t fill = float_to_half(interior_.count(b) ? -kBand : kBand);
        nb->v.fill(fill);
        interior_.erase(b);
        it = bricks_.emplace(b, std::move(nb)).first;
    } else if (it->second.use_count() > 1) {
        it->second = std::make_shared<Brick>(*it->second);  // copy on write: snapshots keep the old one
    }
    it->second->stamp = g_stamp.fetch_add(1);
    return *it->second;
}

void Grid::set(int i, int j, int k, float f) {
    const IVec3 b{floor_div(i, kBrick), floor_div(j, kBrick), floor_div(k, kBrick)};
    mutable_brick(b).v[Brick::index(i - b.x * kBrick, j - b.y * kBrick, k - b.z * kBrick)] =
        float_to_half(std::clamp(f, -kBand, kBand));
}

void Grid::compact(const std::vector<IVec3>& candidates) {
    const std::uint16_t out = float_to_half(kBand), in = float_to_half(-kBand);
    for (const auto& c : candidates) {
        auto it = bricks_.find(c);
        if (it == bricks_.end()) continue;
        const auto& v = it->second->v;
        if (std::all_of(v.begin(), v.end(), [&](std::uint16_t x) { return x == out; })) {
            bricks_.erase(it);
        } else if (std::all_of(v.begin(), v.end(), [&](std::uint16_t x) { return x == in; })) {
            bricks_.erase(it);
            interior_.insert(c);
        }
    }
}

Box3 Grid::bounds() const {
    Box3 b;
    for (const auto& [c, _] : bricks_) {
        b.add(Vec3{double(c.x), double(c.y), double(c.z)} * (kBrick * h_));
        b.add(Vec3{double(c.x + 1), double(c.y + 1), double(c.z + 1)} * (kBrick * h_));
    }
    return b;
}

std::uint64_t Grid::brick_stamp(const IVec3& b) const {
    auto it = bricks_.find(b);
    return it == bricks_.end() ? (interior_.count(b) ? 1u : 0u) : it->second->stamp;
}

std::uint64_t Grid::chunk_key(const IVec3& chunk) const {
    // FNV-1a over the stamps of the chunk's bricks and the ring it reads
    std::uint64_t hsh = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) hsh = (hsh ^ ((v >> (8 * i)) & 0xffu)) * 1099511628211ull;
    };
    mix(std::uint64_t(std::int64_t(chunk.x))), mix(std::uint64_t(std::int64_t(chunk.y))), mix(std::uint64_t(std::int64_t(chunk.z)));
    for (int z = -1; z <= kChunkBricks; ++z)
        for (int y = -1; y <= kChunkBricks; ++y)
            for (int x = -1; x <= kChunkBricks; ++x)
                mix(brick_stamp({chunk.x * kChunkBricks + x, chunk.y * kChunkBricks + y, chunk.z * kChunkBricks + z}));
    mix(std::uint64_t(h_ * 1e9));
    return hsh;
}

// ------------------------------------------------------------------ construction
namespace {
// Fills a brick from f; returns false when the brick lies entirely beyond the band.
bool fill_brick(const std::function<double(const Vec3&)>& f, const IVec3& b, double h, Brick& out) {
    bool any = false;
    for (int z = 0; z < kBrick; ++z)
        for (int y = 0; y < kBrick; ++y)
            for (int x = 0; x < kBrick; ++x) {
                Vec3 p{double(b.x * kBrick + x) * h, double(b.y * kBrick + y) * h, double(b.z * kBrick + z) * h};
                float v = float(std::clamp(f(p) / h, double(-kBand), double(kBand)));
                if (std::abs(v) < kBand) any = true;
                out.v[Brick::index(x, y, z)] = float_to_half(v);
            }
    return any;
}

Grid build(const std::function<double(const Vec3&)>& f, const Box3& bounds, double h) {
    Grid g(h);
    const double bw = kBrick * h, cw = kChunkBricks * bw;
    const double margin = (kBand + 1) * h;
    auto lo = bounds.lo - Vec3{margin, margin, margin}, hi = bounds.hi + Vec3{margin, margin, margin};
    const IVec3 c0{int(std::floor(lo.x / cw)), int(std::floor(lo.y / cw)), int(std::floor(lo.z / cw))};
    const IVec3 c1{int(std::floor(hi.x / cw)), int(std::floor(hi.y / cw)), int(std::floor(hi.z / cw))};
    // two-level classification: chunks, then bricks of ambiguous chunks. A
    // region whose center is farther from the surface than its half diagonal
    // plus the band cannot contain band voxels (f is 1-Lipschitz).
    std::vector<IVec3> chunks;
    for (int z = c0.z; z <= c1.z; ++z)
        for (int y = c0.y; y <= c1.y; ++y)
            for (int x = c0.x; x <= c1.x; ++x) chunks.push_back({x, y, z});
    const double chunk_half = std::sqrt(3.0) * 0.5 * cw, brick_half = std::sqrt(3.0) * 0.5 * (bw - h);
    std::vector<std::vector<IVec3>> amb_bricks(chunks.size()), in_bricks(chunks.size());
    parallel_for(chunks.size(), [&](std::size_t ci) {
        const IVec3 c = chunks[ci];
        Vec3 cc = Vec3{c.x + 0.5, c.y + 0.5, c.z + 0.5} * cw;
        double fc = f(cc);
        if (std::abs(fc) > chunk_half + margin) {
            if (fc < 0)
                for (int z = 0; z < kChunkBricks; ++z)
                    for (int y = 0; y < kChunkBricks; ++y)
                        for (int x = 0; x < kChunkBricks; ++x)
                            in_bricks[ci].push_back({c.x * kChunkBricks + x, c.y * kChunkBricks + y, c.z * kChunkBricks + z});
            return;
        }
        for (int z = 0; z < kChunkBricks; ++z)
            for (int y = 0; y < kChunkBricks; ++y)
                for (int x = 0; x < kChunkBricks; ++x) {
                    IVec3 b{c.x * kChunkBricks + x, c.y * kChunkBricks + y, c.z * kChunkBricks + z};
                    Vec3 bc = Vec3{b.x * kBrick + 3.5, b.y * kBrick + 3.5, b.z * kBrick + 3.5} * h;
                    double fb = f(bc);
                    if (std::abs(fb) > brick_half + margin) {
                        if (fb < 0) in_bricks[ci].push_back(b);
                    } else {
                        amb_bricks[ci].push_back(b);
                    }
                }
    });
    std::vector<IVec3> todo;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        todo.insert(todo.end(), amb_bricks[i].begin(), amb_bricks[i].end());
        for (const auto& b : in_bricks[i]) g.mark_interior(b);
    }
    std::vector<std::shared_ptr<Brick>> filled(todo.size());
    std::vector<char> keep(todo.size(), 0), inside(todo.size(), 0);
    parallel_for(todo.size(), [&](std::size_t i) {
        auto b = std::make_shared<Brick>();
        keep[i] = fill_brick(f, todo[i], h, *b);
        if (!keep[i]) inside[i] = half_to_float(b->v[0]) < 0;
        filled[i] = std::move(b);
    });
    for (std::size_t i = 0; i < todo.size(); ++i) {
        if (keep[i]) {
            Brick& dst = g.mutable_brick(todo[i]);
            dst.v = filled[i]->v;
        } else if (inside[i]) {
            g.mark_interior(todo[i]);
        }
    }
    return g;
}
}  // namespace

Grid from_function(const std::function<double(const Vec3&)>& f, const Box3& bounds, double h) {
    return build(f, bounds, h);
}

Grid from_mesh(const Mesh& m, double h) {
    // angle-weighted pseudonormals of faces, edges and vertices
    std::vector<Vec3> fn(m.F.size()), vn(m.V.size());
    std::unordered_map<std::uint64_t, Vec3> en;
    auto ekey = [](std::uint32_t a, std::uint32_t b) {
        if (a > b) std::swap(a, b);
        return (std::uint64_t(a) << 32) | b;
    };
    for (std::size_t t = 0; t < m.F.size(); ++t) {
        const auto& f = m.F[t];
        const Vec3 &a = m.V[f[0]], &b = m.V[f[1]], &c = m.V[f[2]];
        Vec3 n = normalized(cross(b - a, c - a));
        fn[t] = n;
        const Vec3* P[3] = {&a, &b, &c};
        for (int k = 0; k < 3; ++k) {
            Vec3 e1 = normalized(*P[(k + 1) % 3] - *P[k]), e2 = normalized(*P[(k + 2) % 3] - *P[k]);
            double ang = std::acos(std::clamp(dot(e1, e2), -1.0, 1.0));
            vn[f[k]] += n * ang;
            en[ekey(f[k], f[(k + 1) % 3])] += n;
        }
    }
    Bvh bvh(m);
    auto sdf = [&](const Vec3& p) {
        auto hit = bvh.closest(p);
        if (!hit) return double(kBand) * h;
        const auto& f = m.F[hit->tri];
        Vec3 n = hit->feature == 0   ? fn[hit->tri]
                 : hit->feature <= 3 ? en[ekey(f[hit->feature - 1], f[hit->feature % 3])]
                                     : vn[f[hit->feature - 4]];
        double d = std::sqrt(hit->dist2);
        return dot(p - hit->point, n) < 0 ? -d : d;
    };
    return build(sdf, m.bounds(), h);
}

// ------------------------------------------------------------------ Dual Contouring
namespace {
// Cached brick lookup for the meshing / brush loops.
struct Accessor {
    const Grid& g;
    IVec3 last{INT32_MIN, 0, 0};
    const Brick* brick = nullptr;
    float implied = kBand;
    explicit Accessor(const Grid& grid) : g(grid) {}
    float at(int i, int j, int k) {
        const IVec3 b{floor_div(i, kBrick), floor_div(j, kBrick), floor_div(k, kBrick)};
        if (!(b == last)) {
            last = b;
            auto it = g.bricks().find(b);
            brick = it == g.bricks().end() ? nullptr : it->second.get();
            implied = g.is_interior(b) ? -kBand : kBand;
        }
        if (!brick) return implied;
        return half_table().t[brick->v[Brick::index(i - b.x * kBrick, j - b.y * kBrick, k - b.z * kBrick)]];
    }
};

// A sign change between two lattice values that belongs to the real surface
// (not the artificial jump between a clamped interior and the background).
inline bool crossing(float a, float b) { return (a < 0) != (b < 0) && std::abs(a) + std::abs(b) < 2.5f; }

struct CellSolver {
    Accessor acc;
    double h;
    explicit CellSolver(const Grid& g) : acc(g), h(g.h()) {}

    // Vertex of cell (i, j, k) in mm; false if the cell has no crossing.
    bool vertex(int i, int j, int k, Vec3& out) {
        float v[4][4][4];  // lattice values i-1 .. i+2
        for (int z = 0; z < 4; ++z)
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) v[z][y][x] = acc.at(i - 1 + x, j - 1 + y, k - 1 + z);
        // central-difference gradients at the 8 cell corners
        Vec3 gr[2][2][2];
        for (int z = 0; z < 2; ++z)
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 2; ++x)
                    gr[z][y][x] = Vec3{v[z + 1][y + 1][x + 2] - v[z + 1][y + 1][x], v[z + 1][y + 2][x + 1] - v[z + 1][y][x + 1],
                                       v[z + 2][y + 1][x + 1] - v[z][y + 1][x + 1]} * 0.5;
        auto val = [&](int x, int y, int z) { return v[z + 1][y + 1][x + 1]; };
        auto grad = [&](double x, double y, double z) {  // trilinear in the cell
            Vec3 r;
            for (int c = 0; c < 8; ++c) {
                int cx = c & 1, cy = (c >> 1) & 1, cz = c >> 2;
                double w = (cx ? x : 1 - x) * (cy ? y : 1 - y) * (cz ? z : 1 - z);
                r += gr[cz][cy][cx] * w;
            }
            return r;
        };
        Eigen::Matrix3d A = Eigen::Matrix3d::Zero();
        Eigen::Vector3d b = Eigen::Vector3d::Zero(), mass = Eigen::Vector3d::Zero();
        int n = 0;
        static const int E[12][6] = {{0, 0, 0, 1, 0, 0}, {0, 1, 0, 1, 1, 0}, {0, 0, 1, 1, 0, 1}, {0, 1, 1, 1, 1, 1},
                                     {0, 0, 0, 0, 1, 0}, {1, 0, 0, 1, 1, 0}, {0, 0, 1, 0, 1, 1}, {1, 0, 1, 1, 1, 1},
                                     {0, 0, 0, 0, 0, 1}, {1, 0, 0, 1, 0, 1}, {0, 1, 0, 0, 1, 1}, {1, 1, 0, 1, 1, 1}};
        for (const auto& e : E) {
            float a = val(e[0], e[1], e[2]), c = val(e[3], e[4], e[5]);
            if (!crossing(a, c)) continue;
            double t = a / double(a - c);
            Eigen::Vector3d p(e[0] + t * (e[3] - e[0]), e[1] + t * (e[4] - e[1]), e[2] + t * (e[5] - e[2]));
            Vec3 g3 = normalized(grad(p.x(), p.y(), p.z()));
            Eigen::Vector3d nn(g3.x, g3.y, g3.z);
            A += nn * nn.transpose();
            b += nn * nn.dot(p);
            mass += p;
            ++n;
        }
        if (!n) return false;
        mass /= n;
        // truncated pseudo-inverse: singular values of the normal matrix are
        // sqrt(eigenvalues of A); drop sigma_i < 0.1 sigma_1 (M0 plan 3.7)
        Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(A);
        const Eigen::Vector3d lam = es.eigenvalues();
        const double lmax = lam.maxCoeff();
        Eigen::Vector3d rhs = es.eigenvectors().transpose() * (b - A * mass);
        for (int c = 0; c < 3; ++c) rhs[c] = lam[c] > 0.01 * lmax && lam[c] > 1e-12 ? rhs[c] / lam[c] : 0.0;
        Eigen::Vector3d x = mass + es.eigenvectors() * rhs;
        for (int c = 0; c < 3; ++c) x[c] = std::clamp(x[c], 0.0, 1.0);  // stay in the cell
        out = Vec3{(i + x.x()) * h, (j + x.y()) * h, (k + x.z()) * h};
        return true;
    }
};

struct CellKeyHash {
    std::size_t operator()(const IVec3& c) const noexcept { return IVec3Hash{}(c); }
};

// Contours every edge whose lower lattice point lies in an allocated brick
// accepted by `own`; vertices are shared through `vmap`.
template <class Own>
void contour(const Grid& g, Own&& own, std::unordered_map<IVec3, std::uint32_t, CellKeyHash>& vmap, Mesh& out) {
    CellSolver cs(g);
    Accessor acc(g);
    auto vert = [&](const IVec3& c) -> std::int64_t {
        auto it = vmap.find(c);
        if (it != vmap.end()) return it->second;
        Vec3 p;
        if (!cs.vertex(c.x, c.y, c.z, p)) return -1;
        out.V.push_back(p);
        std::uint32_t id = std::uint32_t(out.V.size() - 1);
        vmap.emplace(c, id);
        return id;
    };
    for (const auto& [bc, brick] : g.bricks()) {
        if (!own(bc)) continue;
        for (int z = 0; z < kBrick; ++z)
            for (int y = 0; y < kBrick; ++y)
                for (int x = 0; x < kBrick; ++x) {
                    const int i = bc.x * kBrick + x, j = bc.y * kBrick + y, k = bc.z * kBrick + z;
                    const float f0 = half_to_float(brick->v[Brick::index(x, y, z)]);
                    for (int a = 0; a < 3; ++a) {
                        const int di = a == 0, dj = a == 1, dk = a == 2;
                        const float f1 = acc.at(i + di, j + dj, k + dk);
                        if (!crossing(f0, f1)) continue;
                        // the four cells around the edge, counter-clockwise seen from +axis
                        const int u = (a + 1) % 3, w = (a + 2) % 3;
                        auto off = [&](int du, int dw) {
                            int d[3] = {0, 0, 0};
                            d[u] = du, d[w] = dw;
                            return IVec3{i + d[0], j + d[1], k + d[2]};
                        };
                        IVec3 cells[4] = {off(-1, -1), off(0, -1), off(0, 0), off(-1, 0)};
                        std::int64_t q[4];
                        bool ok = true;
                        for (int c = 0; c < 4 && ok; ++c) ok = (q[c] = vert(cells[c])) >= 0;
                        if (!ok) continue;
                        if (f0 >= 0) std::swap(q[1], q[3]);  // inside on the far side: normal along -axis
                        // split along the shorter diagonal
                        const Vec3 &p0 = out.V[q[0]], &p1 = out.V[q[1]], &p2 = out.V[q[2]], &p3 = out.V[q[3]];
                        auto T = [](std::int64_t a2, std::int64_t b2, std::int64_t c2) {
                            return Tri{std::uint32_t(a2), std::uint32_t(b2), std::uint32_t(c2)};
                        };
                        if (dot(p2 - p0, p2 - p0) <= dot(p3 - p1, p3 - p1)) {
                            out.F.push_back(T(q[0], q[1], q[2]));
                            out.F.push_back(T(q[0], q[2], q[3]));
                        } else {
                            out.F.push_back(T(q[0], q[1], q[3]));
                            out.F.push_back(T(q[1], q[2], q[3]));
                        }
                    }
                }
    }
}
}  // namespace

Mesh dual_contour_chunk(const Grid& g, const IVec3& chunk) {
    Mesh out;
    std::unordered_map<IVec3, std::uint32_t, CellKeyHash> vmap;
    contour(g, [&](const IVec3& b) { return chunk_of_brick(b) == chunk; }, vmap, out);
    return out;
}

Mesh dual_contour(const Grid& g) {
    Mesh out;
    std::unordered_map<IVec3, std::uint32_t, CellKeyHash> vmap;
    vmap.reserve(g.brick_count() * 128);
    contour(g, [](const IVec3&) { return true; }, vmap, out);
    return out;
}

std::vector<IVec3> chunks_of(const Grid& g) {
    std::unordered_set<IVec3, IVec3Hash> s;
    for (const auto& [b, _] : g.bricks()) s.insert(chunk_of_brick(b));
    return {s.begin(), s.end()};
}

// ------------------------------------------------------------------ brushes
const char* brush_name(Brush b) {
    switch (b) {
        case Brush::Draw: return "draw";
        case Brush::Carve: return "carve";
        case Brush::Smooth: return "smooth";
        case Brush::Grab: return "grab";
        case Brush::Flatten: return "flatten";
    }
    return "draw";
}
bool parse_brush(const std::string& s, Brush& out) {
    for (Brush b : {Brush::Draw, Brush::Carve, Brush::Smooth, Brush::Grab, Brush::Flatten})
        if (s == brush_name(b)) return out = b, true;
    return false;
}

namespace {
// Dense working copy of a brick-aligned block of the grid (one extra brick of
// read-only margin on every side).
struct Block {
    IVec3 b0, nb;     // first brick, brick counts
    int nx, ny, nz;   // voxels
    std::vector<float> f;
    float& at(int x, int y, int z) { return f[(std::size_t(z) * ny + y) * nx + x]; }
    float at(int x, int y, int z) const { return f[(std::size_t(z) * ny + y) * nx + x]; }
    double sample(double x, double y, double z) const {  // trilinear in block voxel coords, clamped
        x = std::clamp(x, 0.0, nx - 1.001), y = std::clamp(y, 0.0, ny - 1.001), z = std::clamp(z, 0.0, nz - 1.001);
        int i = int(x), j = int(y), k = int(z);
        double u = x - i, v = y - j, w = z - k;
        auto L = [](double a, double b, double t) { return a + (b - a) * t; };
        return L(L(L(at(i, j, k), at(i + 1, j, k), u), L(at(i, j + 1, k), at(i + 1, j + 1, k), u), v),
                 L(L(at(i, j, k + 1), at(i + 1, j, k + 1), u), L(at(i, j + 1, k + 1), at(i + 1, j + 1, k + 1), u), v), w);
    }
};

Block load_block(const Grid& g, const Vec3& lo, const Vec3& hi) {
    const double bw = kBrick * g.h();
    Block B;
    B.b0 = {int(std::floor(lo.x / bw)) - 1, int(std::floor(lo.y / bw)) - 1, int(std::floor(lo.z / bw)) - 1};
    IVec3 b1{int(std::floor(hi.x / bw)) + 1, int(std::floor(hi.y / bw)) + 1, int(std::floor(hi.z / bw)) + 1};
    B.nb = {b1.x - B.b0.x + 1, b1.y - B.b0.y + 1, b1.z - B.b0.z + 1};
    B.nx = B.nb.x * kBrick, B.ny = B.nb.y * kBrick, B.nz = B.nb.z * kBrick;
    B.f.assign(std::size_t(B.nx) * B.ny * B.nz, kBand);
    const auto& tab = half_table().t;
    for (int bz = 0; bz < B.nb.z; ++bz)
        for (int by = 0; by < B.nb.y; ++by)
            for (int bx = 0; bx < B.nb.x; ++bx) {
                IVec3 bc{B.b0.x + bx, B.b0.y + by, B.b0.z + bz};
                auto it = g.bricks().find(bc);
                const float implied = g.is_interior(bc) ? -kBand : kBand;
                for (int z = 0; z < kBrick; ++z)
                    for (int y = 0; y < kBrick; ++y)
                        for (int x = 0; x < kBrick; ++x)
                            B.at(bx * kBrick + x, by * kBrick + y, bz * kBrick + z) =
                                it == g.bricks().end() ? implied : tab[it->second->v[Brick::index(x, y, z)]];
            }
    return B;
}

// Writes the inner bricks back; returns the bricks whose content changed.
std::vector<IVec3> store_block(Grid& g, const Block& B, const Block& before) {
    std::vector<IVec3> changed;
    for (int bz = 1; bz < B.nb.z - 1; ++bz)
        for (int by = 1; by < B.nb.y - 1; ++by)
            for (int bx = 1; bx < B.nb.x - 1; ++bx) {
                std::array<std::uint16_t, kBrickVoxels> hv;
                bool diff = false;
                for (int z = 0; z < kBrick; ++z)
                    for (int y = 0; y < kBrick; ++y)
                        for (int x = 0; x < kBrick; ++x) {
                            const int X = bx * kBrick + x, Y = by * kBrick + y, Z = bz * kBrick + z;
                            float v = std::clamp(B.at(X, Y, Z), -kBand, kBand);
                            hv[Brick::index(x, y, z)] = float_to_half(v);
                            if (hv[Brick::index(x, y, z)] != float_to_half(std::clamp(before.at(X, Y, Z), -kBand, kBand)))
                                diff = true;
                        }
                if (!diff) continue;
                IVec3 bc{B.b0.x + bx, B.b0.y + by, B.b0.z + bz};
                g.mutable_brick(bc).v = hv;
                changed.push_back(bc);
            }
    return changed;
}

// Reinitialisation inside [lo, hi] (block voxel coordinates):
//  1. near the surface (|f| < 1.5 h, stencil unclamped): f <- f / |grad f|,
//     the first-order correction that restores |grad f| = 1 (Chopp 1993);
//  2. the outer band (1.5 h <= |f| <= kBand h) is rebuilt from the inner
//     values as a 26-neighbour chamfer distance with weights 1, sqrt 2,
//     sqrt 3 (Borgefors 1986), two passes. Without step 2 the band edge,
//     whose stencil touches clamped values, drifts and eventually breaks the
//     crossing test near the surface.
void reinit(Block& B, IVec3 lo, IVec3 hi) {
    lo = {std::max(lo.x, 1), std::max(lo.y, 1), std::max(lo.z, 1)};
    hi = {std::min(hi.x, B.nx - 2), std::min(hi.y, B.ny - 2), std::min(hi.z, B.nz - 2)};
    constexpr float kNear = 1.5f;
    {
        const Block src = B;
        for (int z = lo.z; z <= hi.z; ++z)
            for (int y = lo.y; y <= hi.y; ++y)
                for (int x = lo.x; x <= hi.x; ++x) {
                    const float f = src.at(x, y, z);
                    if (std::abs(f) >= kNear) continue;
                    const float n[6] = {src.at(x + 1, y, z), src.at(x - 1, y, z), src.at(x, y + 1, z),
                                        src.at(x, y - 1, z), src.at(x, y, z + 1), src.at(x, y, z - 1)};
                    bool clamped = false;
                    for (float v : n) clamped = clamped || std::abs(v) >= kBand;
                    if (clamped) continue;
                    const double gx = 0.5 * (n[0] - n[1]), gy = 0.5 * (n[2] - n[3]), gz = 0.5 * (n[4] - n[5]);
                    const double gn = std::sqrt(gx * gx + gy * gy + gz * gz);
                    if (gn > 0.2) B.at(x, y, z) = float(f / gn);
                }
    }
    static const float W[3] = {1.0f, 1.41421356f, 1.73205081f};
    for (int pass = 0; pass < 2; ++pass) {
        const Block src = B;
        for (int z = lo.z; z <= hi.z; ++z)
            for (int y = lo.y; y <= hi.y; ++y)
                for (int x = lo.x; x <= hi.x; ++x) {
                    const float f = src.at(x, y, z);
                    if (std::abs(f) < kNear) continue;
                    const bool neg = f < 0;
                    float best = kBand;
                    for (int dz = -1; dz <= 1; ++dz)
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx) {
                                const int k = std::abs(dx) + std::abs(dy) + std::abs(dz);
                                if (!k) continue;
                                const float v = src.at(x + dx, y + dy, z + dz);
                                if ((v < 0) != neg) continue;  // only through same-side neighbours
                                best = std::min(best, std::abs(v) + W[k - 1]);
                            }
                    B.at(x, y, z) = neg ? -best : best;
                }
    }
}
}  // namespace

std::vector<IVec3> apply_stroke(Grid& g, const Stroke& s) {
    if (s.path.empty() || !(s.radius > 0)) return {};
    const double h = g.h(), R = s.radius, st = std::clamp(s.strength, 0.0, 1.0);
    // resample the path into dabs, spacing R/4
    std::vector<Vec3> dabs{s.path.front()};
    const double spacing = R / 4;
    double carry = 0;
    for (std::size_t i = 1; i < s.path.size(); ++i) {
        Vec3 a = s.path[i - 1], b = s.path[i];
        double L = norm(b - a);
        double t = spacing - carry;
        while (t <= L) {
            dabs.push_back(a + (b - a) * (t / L));
            t += spacing;
        }
        carry = L - (t - spacing);
    }
    if (s.brush == Brush::Grab && s.path.size() > 1 && !(dabs.back() == s.path.back())) dabs.push_back(s.path.back());

    // flatten: plane through the first dab, normal = outward field gradient there
    Vec3 pn = normalized(g.gradient(s.path.front()));
    double pd = dot(pn, s.path.front());

    std::unordered_set<IVec3, IVec3Hash> touched;
    for (std::size_t di = 0; di < dabs.size(); ++di) {
        const Vec3 c = dabs[di];
        Vec3 grab{};
        if (s.brush == Brush::Grab) {
            if (di == 0) continue;
            grab = dabs[di] - dabs[di - 1];
        }
        const Vec3 cc = s.brush == Brush::Grab ? dabs[di - 1] : c;
        const double reach = R + (kBand + 1) * h + norm(grab);
        Block B = load_block(g, cc - Vec3{reach, reach, reach}, cc + Vec3{reach, reach, reach});
        const Block before = B;
        // sub-steps keep every update below half a voxel (the band stays valid)
        double amount = 0;
        if (s.brush == Brush::Draw || s.brush == Brush::Carve) amount = st * spacing / 4;  // mm per dab
        if (s.brush == Brush::Grab) amount = norm(grab);
        const int sub = std::max(1, int(std::ceil(amount / (0.5 * h))));
        const Vec3 origin = Vec3{double(B.b0.x), double(B.b0.y), double(B.b0.z)} * (kBrick * h);
        for (int ss = 0; ss < sub; ++ss) {
            Block src = B;
            for (int z = 1; z < B.nz - 1; ++z)
                for (int y = 1; y < B.ny - 1; ++y)
                    for (int x = 1; x < B.nx - 1; ++x) {
                        const Vec3 p = origin + Vec3{double(x), double(y), double(z)} * h;
                        const double q = norm(p - cc) / R;
                        if (q >= 1.0) continue;
                        const double w = wendland(q);
                        float& f = B.at(x, y, z);
                        switch (s.brush) {
                            case Brush::Draw: f -= float(w * amount / sub / h); break;
                            case Brush::Carve: f += float(w * amount / sub / h); break;
                            case Brush::Smooth: {
                                if (std::abs(src.at(x, y, z)) >= kBand) break;
                                double avg = (src.at(x + 1, y, z) + src.at(x - 1, y, z) + src.at(x, y + 1, z) +
                                              src.at(x, y - 1, z) + src.at(x, y, z + 1) + src.at(x, y, z - 1)) / 6.0;
                                f = float(src.at(x, y, z) + 0.5 * st * w * (avg - src.at(x, y, z)));
                                break;
                            }
                            case Brush::Grab: {  // semi-Lagrangian: f(x) <- f(x - w delta)
                                const Vec3 d = grab * (w / sub / h);
                                f = float(src.sample(x - d.x, y - d.y, z - d.z));
                                break;
                            }
                            case Brush::Flatten: {
                                if (std::abs(src.at(x, y, z)) >= kBand) break;
                                double fp = (dot(pn, p) - pd) / h;
                                f = float(src.at(x, y, z) + 0.5 * st * w * (fp - src.at(x, y, z)));
                                break;
                            }
                        }
                    }
            // voxels the dab can influence (+ the band), in block coordinates
            const int rv = int(std::ceil(reach / h));
            const Vec3 cb = (cc - origin) / h;
            reinit(B, {int(cb.x) - rv, int(cb.y) - rv, int(cb.z) - rv}, {int(cb.x) + rv, int(cb.y) + rv, int(cb.z) + rv});
        }
        for (const auto& b : store_block(g, B, before)) touched.insert(b);
    }
    std::vector<IVec3> out(touched.begin(), touched.end());
    g.compact(out);
    return out;
}

}  // namespace anvil::sdf
