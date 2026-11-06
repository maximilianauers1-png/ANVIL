#include "anvil/sdf.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>

#include "anvil/bvh.hpp"
#include "anvil/sort.hpp"
#include "anvil/tolerance.hpp"
#include "anvil/winding.hpp"

namespace anvil::sdf {

namespace {
// A surface exactly through lattice points (f == 0, e.g. a flat top at z = 0)
// puts several cell vertices on the same point: zero-area triangles. Zero is
// stored as +2^-10 h instead (consistent with "0 counts as outside").
constexpr float kZeroNudge = 1.0f / 1024.0f;
// Solid islands of at most 3^3 voxels (0.15 mm at 50 um) cannot be printed
// (resin XY pixel ~18-50 um, but a free-floating speck has no support) and
// are sign noise of robust voxelization; see remove_specks.
constexpr std::size_t kSpeckVoxels = 27;
}  // namespace

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
    return *it->second;
}

void Grid::touch_mesh(const IVec3& b) {
    auto it = bricks_.find(b);
    if (it != bricks_.end()) mutable_brick(b).stamp = g_stamp.fetch_add(1);
}

void Grid::set(int i, int j, int k, float f) {
    const IVec3 b{floor_div(i, kBrick), floor_div(j, kBrick), floor_div(k, kBrick)};
    Brick& br = mutable_brick(b);
    br.v[Brick::index(i - b.x * kBrick, j - b.y * kBrick, k - b.z * kBrick)] = float_to_half(std::clamp(f, -kBand, kBand));
    br.stamp = g_stamp.fetch_add(1);
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
// Fills all voxels of brick b (index Brick::index) with signed distances (mm).
using BrickFiller = std::function<void(const IVec3& b, double h, double* vals)>;

bool fill_brick(const std::function<double(const Vec3&)>& f, const BrickFiller* filler, const IVec3& b, double h, Brick& out) {
    bool any = false, has_neg = false, has_pos = false;
    double vals[kBrickVoxels];
    if (filler) (*filler)(b, h, vals);
    for (int z = 0; z < kBrick; ++z)
        for (int y = 0; y < kBrick; ++y)
            for (int x = 0; x < kBrick; ++x) {
                Vec3 p{double(b.x * kBrick + x) * h, double(b.y * kBrick + y) * h, double(b.z * kBrick + z) * h};
                const double fv = filler ? vals[Brick::index(x, y, z)] : f(p);
                float v = float(std::clamp(fv / h, double(-kBand), double(kBand)));
                if (v == 0.0f) v = kZeroNudge;  // see kZeroNudge
                if (std::abs(v) < kBand) any = true;
                (v < 0 ? has_neg : has_pos) = true;
                out.v[Brick::index(x, y, z)] = float_to_half(v);
            }
    return any || (has_neg && has_pos);  // a sign change must be contoured even at clamped values
}

Grid build(const std::function<double(const Vec3&)>& f, const Box3& bounds, double h, double slack = 0.0,
           const BrickFiller* filler = nullptr) {
    Grid g(h);
    const double bw = kBrick * h, cw = kChunkBricks * bw;
    const double margin = (kBand + 1) * h + slack;
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
        keep[i] = fill_brick(f, filler, todo[i], h, *b);
        if (!keep[i]) inside[i] = half_to_float(b->v[0]) < 0;
        filled[i] = std::move(b);
    });
    for (std::size_t i = 0; i < todo.size(); ++i) {
        if (keep[i]) {
            Brick& dst = g.mutable_brick(todo[i]);
            dst.v = filled[i]->v;
            dst.stamp = g_stamp.fetch_add(1);
        } else if (inside[i]) {
            g.mark_interior(todo[i]);
        }
    }
    return g;
}
}  // namespace

Grid from_function(const std::function<double(const Vec3&)>& f, const Box3& bounds, double h, double slack) {
    return build(f, bounds, h, slack);
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

std::size_t fill_cavities(Grid& g) {
    // Flood fill of the outside through the positive voxels of the allocated
    // bricks, seeded where a positive voxel borders an unallocated brick that
    // is not interior (implied +kBand: outside). Positive voxels it cannot
    // reach are enclosed voids; their values are mirrored to negative.
    std::vector<IVec3> coords;
    std::unordered_map<IVec3, std::uint32_t, IVec3Hash> index;
    for (const auto& [c, _] : g.bricks()) index.emplace(c, std::uint32_t(coords.size())), coords.push_back(c);
    std::sort(coords.begin(), coords.end(), [](const IVec3& a, const IVec3& b) { return std::tie(a.z, a.y, a.x) < std::tie(b.z, b.y, b.x); });
    for (std::uint32_t i = 0; i < coords.size(); ++i) index[coords[i]] = i;
    std::vector<const Brick*> br(coords.size());
    for (std::uint32_t i = 0; i < coords.size(); ++i) br[i] = g.bricks().at(coords[i]).get();
    std::vector<std::uint8_t> seen(coords.size() * kBrickVoxels, 0);
    std::vector<std::uint64_t> stack;  // brick index << 9 | voxel
    auto pos = [&](std::uint32_t b, int v) { return half_to_float(br[b]->v[std::size_t(v)]) > 0; };
    auto push = [&](std::uint32_t b, int v) {
        auto& sv = seen[std::size_t(b) * kBrickVoxels + std::size_t(v)];
        if (sv || !pos(b, v)) return;
        sv = 1;
        stack.push_back((std::uint64_t(b) << 9) | std::uint64_t(v));
    };
    const int dirs[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    // neighbour of voxel (b, x, y, z) in direction d: same brick or the adjacent one
    auto step = [&](std::uint32_t b, int x, int y, int z, int d, std::uint32_t& nb, int& nv) {
        int c[3] = {x + dirs[d][0], y + dirs[d][1], z + dirs[d][2]};
        IVec3 bc = coords[b];
        int* bcp[3] = {&bc.x, &bc.y, &bc.z};
        for (int k = 0; k < 3; ++k) {
            if (c[k] < 0) c[k] += kBrick, --*bcp[k];
            else if (c[k] >= kBrick) c[k] -= kBrick, ++*bcp[k];
        }
        nv = Brick::index(c[0], c[1], c[2]);
        if (bc == coords[b]) {
            nb = b;
            return 1;  // inside the brick
        }
        auto it = index.find(bc);
        if (it != index.end()) {
            nb = it->second;
            return 1;
        }
        return g.is_interior(bc) ? 0 : -1;  // 0: solid, -1: outside
    };
    for (std::uint32_t b = 0; b < coords.size(); ++b)
        for (int z = 0; z < kBrick; ++z)
            for (int y = 0; y < kBrick; ++y)
                for (int x = 0; x < kBrick; ++x) {
                    if (x != 0 && y != 0 && z != 0 && x != kBrick - 1 && y != kBrick - 1 && z != kBrick - 1) continue;
                    const int v = Brick::index(x, y, z);
                    for (int d = 0; d < 6; ++d) {
                        std::uint32_t nb;
                        int nv;
                        if (step(b, x, y, z, d, nb, nv) < 0) {
                            push(b, v);
                            break;
                        }
                    }
                }
    while (!stack.empty()) {
        const std::uint64_t e = stack.back();
        stack.pop_back();
        const auto b = std::uint32_t(e >> 9);
        const int v = int(e & 511), x = v % kBrick, y = (v / kBrick) % kBrick, z = v / (kBrick * kBrick);
        for (int d = 0; d < 6; ++d) {
            std::uint32_t nb;
            int nv;
            if (step(b, x, y, z, d, nb, nv) > 0) push(nb, nv);
        }
    }
    std::size_t filled = 0;
    for (std::uint32_t b = 0; b < coords.size(); ++b) {
        bool any = false;
        for (int v = 0; v < kBrickVoxels; ++v)
            if (!seen[std::size_t(b) * kBrickVoxels + std::size_t(v)] && pos(b, v)) any = true;
        if (!any) continue;
        Brick& m = g.mutable_brick(coords[b]);
        br[b] = &m;
        for (int v = 0; v < kBrickVoxels; ++v) {
            const float f = half_to_float(m.v[std::size_t(v)]);
            if (f > 0 && !seen[std::size_t(b) * kBrickVoxels + std::size_t(v)]) m.v[std::size_t(v)] = float_to_half(-f), ++filled;
        }
        g.touch_mesh(coords[b]);
    }
    return filled;
}

std::size_t remove_specks(Grid& g, std::size_t max_voxels) {
    // Components of negative voxels (6-neighbourhood, within allocated bricks)
    // with at most max_voxels voxels that touch no interior brick are flipped
    // to positive: sub-voxel specks the sign test leaves next to skin folds.
    std::vector<IVec3> coords;
    for (const auto& [c, _] : g.bricks()) coords.push_back(c);
    std::sort(coords.begin(), coords.end(), [](const IVec3& a, const IVec3& b) { return std::tie(a.z, a.y, a.x) < std::tie(b.z, b.y, b.x); });
    std::unordered_map<IVec3, std::uint32_t, IVec3Hash> index;
    for (std::uint32_t i = 0; i < coords.size(); ++i) index[coords[i]] = i;
    std::vector<const Brick*> br(coords.size());
    for (std::uint32_t i = 0; i < coords.size(); ++i) br[i] = g.bricks().at(coords[i]).get();
    std::vector<std::uint8_t> seen(coords.size() * kBrickVoxels, 0);
    auto neg = [&](std::uint32_t b, int v) { return half_to_float(br[b]->v[std::size_t(v)]) < 0; };
    const int dirs[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    std::vector<std::pair<std::uint32_t, int>> flip;
    for (std::uint32_t b0 = 0; b0 < coords.size(); ++b0)
        for (int v0 = 0; v0 < kBrickVoxels; ++v0) {
            if (seen[std::size_t(b0) * kBrickVoxels + std::size_t(v0)] || !neg(b0, v0)) continue;
            std::vector<std::pair<std::uint32_t, int>> comp{{b0, v0}};
            seen[std::size_t(b0) * kBrickVoxels + std::size_t(v0)] = 1;
            bool large = false;
            for (std::size_t q = 0; q < comp.size(); ++q) {
                const auto [b, v] = comp[q];
                const int x = v % kBrick, y = (v / kBrick) % kBrick, z = v / (kBrick * kBrick);
                for (const auto& d : dirs) {
                    int c[3] = {x + d[0], y + d[1], z + d[2]};
                    IVec3 bc = coords[b];
                    int* bcp[3] = {&bc.x, &bc.y, &bc.z};
                    for (int k = 0; k < 3; ++k) {
                        if (c[k] < 0) c[k] += kBrick, --*bcp[k];
                        else if (c[k] >= kBrick) c[k] -= kBrick, ++*bcp[k];
                    }
                    std::uint32_t nb = b;
                    if (!(bc == coords[b])) {
                        auto it = index.find(bc);
                        if (it == index.end()) {
                            if (g.is_interior(bc)) large = true;
                            continue;
                        }
                        nb = it->second;
                    }
                    const int nv = Brick::index(c[0], c[1], c[2]);
                    auto& sv = seen[std::size_t(nb) * kBrickVoxels + std::size_t(nv)];
                    if (sv || !neg(nb, nv)) continue;
                    sv = 1;
                    comp.push_back({nb, nv});
                }
                if (comp.size() > max_voxels) large = true;
                if (large) break;
            }
            if (large) {
                // mark the rest of the component visited without collecting it
                for (std::size_t q = 0; q < comp.size(); ++q) {
                    const auto [b, v] = comp[q];
                    const int x = v % kBrick, y = (v / kBrick) % kBrick, z = v / (kBrick * kBrick);
                    for (const auto& d : dirs) {
                        int c[3] = {x + d[0], y + d[1], z + d[2]};
                        IVec3 bc = coords[b];
                        int* bcp[3] = {&bc.x, &bc.y, &bc.z};
                        for (int k = 0; k < 3; ++k) {
                            if (c[k] < 0) c[k] += kBrick, --*bcp[k];
                            else if (c[k] >= kBrick) c[k] -= kBrick, ++*bcp[k];
                        }
                        auto it = index.find(bc);
                        if (it == index.end()) continue;
                        const int nv = Brick::index(c[0], c[1], c[2]);
                        auto& sv = seen[std::size_t(it->second) * kBrickVoxels + std::size_t(nv)];
                        if (sv || !neg(it->second, nv)) continue;
                        sv = 1;
                        comp.push_back({it->second, nv});
                    }
                }
                continue;
            }
            flip.insert(flip.end(), comp.begin(), comp.end());
        }
    std::sort(flip.begin(), flip.end());
    for (std::size_t i = 0; i < flip.size();) {
        const std::uint32_t b = flip[i].first;
        Brick& m = g.mutable_brick(coords[b]);
        for (; i < flip.size() && flip[i].first == b; ++i)
            m.v[std::size_t(flip[i].second)] = float_to_half(std::abs(half_to_float(m.v[std::size_t(flip[i].second)])));
        g.touch_mesh(coords[b]);
    }
    return flip.size();
}

std::size_t resolve_ambiguous_faces(Grid& g) {
    // A lattice cell (2x2x2 voxels) whose inside or outside corners form more
    // than one component along the cell edges holds two surface sheets: the
    // contour gives it one vertex, which joins the sheets (non-manifold edges,
    // folds after splitting). Such cells are gaps or necks thinner than one
    // voxel -- below what resin printers resolve. They are closed towards the
    // solid: split inside corners are joined by making the outside corner
    // with the smallest value inside; split outside corners keep only their
    // largest component. Values only ever go from outside to inside, so the
    // rounds terminate. Corners of such a cell lie within one voxel of the
    // surface (f is 1-Lipschitz), so only cells whose first corner has
    // |f| <= sqrt 3 h are visited.
    static const int E[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    std::size_t total = 0;
    for (int round = 0; round < 8; ++round) {
        std::vector<IVec3> coords;
        for (const auto& [c, _] : g.bricks()) coords.push_back(c);
        std::sort(coords.begin(), coords.end(), [](const IVec3& a, const IVec3& b) { return std::tie(a.z, a.y, a.x) < std::tie(b.z, b.y, b.x); });
        std::vector<std::vector<std::array<int, 3>>> fixes(coords.size());
        parallel_for(coords.size(), [&](std::size_t bi) {
            const IVec3 b = coords[bi];
            for (int z = 0; z < kBrick; ++z)
                for (int y = 0; y < kBrick; ++y)
                    for (int x = 0; x < kBrick; ++x) {
                        const int i = b.x * kBrick + x, j = b.y * kBrick + y, k = b.z * kBrick + z;
                        if (std::abs(g.at(i, j, k)) > 1.7321f) continue;
                        float v[8];
                        int npos = 0;
                        for (int c = 0; c < 8; ++c) {
                            v[c] = g.at(i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1));
                            npos += v[c] > 0;
                        }
                        if (npos == 0 || npos == 8) continue;
                        // components per sign along the 12 cell edges
                        int comp[8];
                        for (int c = 0; c < 8; ++c) comp[c] = c;
                        auto find = [&](int c) { while (comp[c] != c) c = comp[c] = comp[comp[c]]; return c; };
                        for (const auto& e : E)
                            if ((v[e[0]] > 0) == (v[e[1]] > 0)) comp[find(e[0])] = find(e[1]);
                        int size[8] = {}, roots_pos = 0, roots_neg = 0;
                        for (int c = 0; c < 8; ++c) ++size[find(c)];
                        for (int c = 0; c < 8; ++c)
                            if (find(c) == c) (v[c] > 0 ? roots_pos : roots_neg) += 1;
                        if (roots_pos <= 1 && roots_neg <= 1) continue;
                        auto at = [&](int c) { return std::array<int, 3>{i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1)}; };
                        if (roots_neg > 1) {
                            int best = -1;
                            for (int c = 0; c < 8; ++c)
                                if (v[c] > 0 && (best < 0 || v[c] < v[best])) best = c;
                            fixes[bi].push_back(at(best));
                        } else {
                            int big = -1;
                            for (int c = 0; c < 8; ++c)
                                if (v[c] > 0 && (big < 0 || size[find(c)] > size[find(big)])) big = c;
                            for (int c = 0; c < 8; ++c)
                                if (v[c] > 0 && find(c) != find(big)) fixes[bi].push_back(at(c));
                        }
                    }
        });
        std::size_t n = 0;
        for (const auto& f : fixes)
            for (const auto& p : f)
                if (const float v = g.at(p[0], p[1], p[2]); v > 0) {
                    // mirrored (at least 1/4 voxel): the contour crosses the joined
                    // edges inside them instead of at the lattice point itself
                    g.set(p[0], p[1], p[2], -std::max(v, 0.25f));
                    g.touch_mesh({floor_div(p[0], kBrick), floor_div(p[1], kBrick), floor_div(p[2], kBrick)});
                    ++n;
                }
        total += n;
        if (n == 0) break;
    }
    return total;
}

std::size_t clamp_crossings(Grid& g) {
    // Contouring ignores lattice edges with |a| + |b| >= 2.5 h (band-edge
    // jumps between clamped values). Where the signs really change, the zero
    // set lies on the edge, so each end is at most h from it: clamp both to
    // |f| <= h. Sign flips of the clean-up passes and bricks next to implied
    // (unallocated) ones otherwise leave such edges uncontoured -- holes.
    std::vector<IVec3> coords;
    for (const auto& [c, _] : g.bricks()) coords.push_back(c);
    std::sort(coords.begin(), coords.end(), [](const IVec3& a, const IVec3& b) { return std::tie(a.z, a.y, a.x) < std::tie(b.z, b.y, b.x); });
    std::vector<std::vector<std::array<int, 3>>> fixes(coords.size());
    parallel_for(coords.size(), [&](std::size_t bi) {
        const IVec3 b = coords[bi];
        for (int z = 0; z < kBrick; ++z)
            for (int y = 0; y < kBrick; ++y)
                for (int x = 0; x < kBrick; ++x) {
                    const int i = b.x * kBrick + x, j = b.y * kBrick + y, k = b.z * kBrick + z;
                    const float v = g.at(i, j, k);
                    const int nb[6][3] = {{i - 1, j, k}, {i + 1, j, k}, {i, j - 1, k}, {i, j + 1, k}, {i, j, k - 1}, {i, j, k + 1}};
                    for (const auto& c : nb) {
                        const float w = g.at(c[0], c[1], c[2]);
                        if ((v < 0) == (w < 0)) continue;
                        if (std::abs(v) > 1.0f) fixes[bi].push_back({i, j, k});
                        if (std::abs(w) > 1.0f) fixes[bi].push_back({c[0], c[1], c[2]});  // may lie in an implied brick
                    }
                }
    });
    std::size_t n = 0;
    for (const auto& f : fixes)
        for (const auto& p : f)
            if (const float v = g.at(p[0], p[1], p[2]); std::abs(v) > 1.0f) {
                g.set(p[0], p[1], p[2], v < 0 ? -1.0f : 1.0f);
                g.touch_mesh({floor_div(p[0], kBrick), floor_div(p[1], kBrick), floor_div(p[2], kBrick)});
                ++n;
            }
    return n;
}

std::size_t clean_for_print(Grid& g) {
    // each step can create work for the others (a joined diagonal may close a
    // void, a removed speck may leave a checkerboard face): iterate to a fixed
    // point; 6 rounds were never exceeded in the M4 tests
    std::size_t total = 0;
    for (int round = 0; round < 6; ++round) {
        const std::size_t n = fill_cavities(g) + remove_specks(g, kSpeckVoxels) + resolve_ambiguous_faces(g);
        total += n;
        if (n == 0) break;
    }
    return total + clamp_crossings(g);  // signs are final: make every crossing contourable
}

Grid from_mesh_robust(const Mesh& input, double h, std::size_t* capped) {
    // Sign: generalized winding number |w| > 1/2 of the boundary-capped soup
    // (ADR 0018). After capping, w is integer valued off the surface, so it is
    // constant inside any ball that contains no triangle: two lattice
    // neighbours p, q (|p - q| = h) share the sign if the unsigned distance of
    // either exceeds h. Inside a brick w is therefore evaluated once per
    // region the surface separates (typically 1-3 times instead of 512).
    Mesh m = input;
    const std::size_t added = cap_boundaries(m);
    if (capped) *capped = added;
    const WindingNumber wn(m, tol::kWindingBeta);
    auto inside = [&](const Vec3& p) { return std::abs(wn(p)) > 0.5; };
    // A lattice point on the surface itself (models are fitted to z = 0 and a
    // height on the lattice: flat soles and head tops lie on lattice planes)
    // reads w = 1/2. Test a point 1 % of a voxel off it in a generic direction
    // (not parallel to the axes or to 45-degree faces) instead.
    const Vec3 off = normalized(Vec3{0.5773, 0.6181, 0.5331}) * (0.01 * h);
    auto inside_at = [&](const Vec3& p, double dist) { return inside(dist < 0.01 * h ? p + off : p); };
    // Values are distances to the boundary of the solid only: a triangle is on
    // it if the inside test differs a quarter voxel in front of and behind it
    // at any of 7 sample points (centroid, near the corners, near the edge
    // midpoints; a triangle may be only partly buried). Sheets inside the
    // solid (buried shells, folds of posed skins) would otherwise pull |f|
    // towards 0 just under the real surface and collapse contour vertices.
    // Skipping bricks and propagating signs keep using the distance to all
    // triangles: a lower bound of the boundary distance, so both stay valid
    // even where a boundary piece was missed (it then only biases the value).
    Mesh boundary;
    boundary.V = m.V;
    {
        std::vector<char> keep(m.F.size(), 0);
        const double eps = 0.25 * h;
        static const double bary[7][3] = {{1 / 3., 1 / 3., 1 / 3.}, {0.8, 0.1, 0.1}, {0.1, 0.8, 0.1}, {0.1, 0.1, 0.8},
                                          {0.45, 0.45, 0.1}, {0.1, 0.45, 0.45}, {0.45, 0.1, 0.45}};
        parallel_for(m.F.size(), [&](std::size_t t) {
            const auto& f = m.F[t];
            const Vec3 &a = m.V[f[0]], &b = m.V[f[1]], &c = m.V[f[2]];
            const Vec3 n = normalized(cross(b - a, c - a));
            if (norm(n) == 0) return;  // degenerate: no area, no boundary
            for (const auto& w : bary) {
                const Vec3 q = a * w[0] + b * w[1] + c * w[2];
                if (inside(q + n * eps) != inside(q - n * eps)) {
                    keep[t] = 1;
                    return;
                }
            }
        });
        for (std::size_t t = 0; t < m.F.size(); ++t)
            if (keep[t]) boundary.F.push_back(m.F[t]);
        if (boundary.F.empty()) boundary.F = m.F;
    }
    const Bvh bvh(m), bvh_b(boundary);
    auto f = [&](const Vec3& p) {
        auto hit = bvh.closest(p);
        const double d = hit ? std::sqrt(hit->dist2) : double(kBand + 1) * h;
        return inside(p) ? -d : d;
    };
    const BrickFiller filler = [&](const IVec3& b, double hh, double* vals) {
        // signs on the brick plus a one-voxel halo (10^3), so that sign changes
        // across brick faces are seen from both sides
        constexpr int kH = kBrick + 2, kHV = kH * kH * kH;
        auto hid = [](int x, int y, int z) { return ((z + 1) * kH + (y + 1)) * kH + (x + 1); };  // x, y, z in -1..8
        const double dmax = double(kBand + 1) * hh;
        // strictly more than one voxel (rounding of d must not let a sign through
        // a voxel that lies on the surface)
        const double reach = hh * (1 + 1e-6);
        double d[kHV];                 // distance to all triangles (sign propagation)
        signed char sg[kHV] = {};
        auto pos = [&](int x, int y, int z) {
            return Vec3{double(b.x * kBrick + x) * hh, double(b.y * kBrick + y) * hh, double(b.z * kBrick + z) * hh};
        };
        for (int z = -1; z <= kBrick; ++z)
            for (int y = -1; y <= kBrick; ++y)
                for (int x = -1; x <= kBrick; ++x) {
                    auto hit = bvh.closest(pos(x, y, z), dmax);
                    d[hid(x, y, z)] = hit ? std::sqrt(hit->dist2) : dmax;
                }
        int queue[kHV];
        for (int z0 = -1; z0 <= kBrick; ++z0)
            for (int y0 = -1; y0 <= kBrick; ++y0)
                for (int x0 = -1; x0 <= kBrick; ++x0) {
                    const int s0 = hid(x0, y0, z0);
                    if (sg[s0]) continue;
                    sg[s0] = inside_at(pos(x0, y0, z0), d[s0]) ? -1 : 1;
                    int qh = 0, qt = 0;
                    queue[qt++] = s0;
                    while (qh < qt) {
                        const int i = queue[qh++];
                        const int x = i % kH - 1, y = (i / kH) % kH - 1, z = i / (kH * kH) - 1;
                        const int nb[6][3] = {{x - 1, y, z}, {x + 1, y, z}, {x, y - 1, z}, {x, y + 1, z}, {x, y, z - 1}, {x, y, z + 1}};
                        for (const auto& c : nb) {
                            if (c[0] < -1 || c[1] < -1 || c[2] < -1 || c[0] > kBrick || c[1] > kBrick || c[2] > kBrick) continue;
                            const int j = hid(c[0], c[1], c[2]);
                            if (sg[j] || !(d[i] > reach || d[j] > reach)) continue;
                            sg[j] = sg[i];
                            queue[qt++] = j;
                        }
                    }
                }
        for (int z = 0; z < kBrick; ++z)
            for (int y = 0; y < kBrick; ++y)
                for (int x = 0; x < kBrick; ++x) {
                    const int i = hid(x, y, z);
                    auto hb = bvh_b.closest(pos(x, y, z), dmax);
                    double di = hb ? std::sqrt(hb->dist2) : dmax;
                    // the boundary lies between a voxel and a face neighbour of the
                    // other sign: its distance is at most h (also where a boundary
                    // piece was missed by the triangle classification)
                    const int nb[6][3] = {{x - 1, y, z}, {x + 1, y, z}, {x, y - 1, z}, {x, y + 1, z}, {x, y, z - 1}, {x, y, z + 1}};
                    for (const auto& c : nb)
                        if (sg[hid(c[0], c[1], c[2])] != sg[i]) di = std::min(di, hh);
                    // a voxel exactly on a sheet keeps its sign (no zero nudge outwards)
                    di = std::max(di, double(kZeroNudge) * hh);
                    vals[Brick::index(x, y, z)] = sg[i] < 0 ? -di : di;
                }
    };
    Grid g = build(f, m.bounds(), h, 0.0, &filler);
    clean_for_print(g);  // folds of posed skins enclose pockets with w = 0, sign noise, pinches
    return g;
}

// ------------------------------------------------------------------ grid booleans
double voxel_step(double wanted) {
    static const double steps[] = {0.01, 0.02, 0.03, 0.05, 0.075, 0.1, 0.15, 0.2, 0.3, 0.5, 0.75, 1, 1.5, 2, 3, 5};
    double best = steps[0];
    for (double s : steps)
        if (s <= wanted * 1.0000001) best = s;  // the largest standard step not coarser than wanted
    return best;
}

Grid combine(const Grid& a, const Grid& b_in, GridOp op) {
    const double h = a.h();
    // resample b onto a's lattice when the voxel sizes differ
    Grid resampled(h);
    const Grid* bp = &b_in;
    if (std::abs(b_in.h() - h) > 1e-12 * h) {
        resampled = from_function([&](const Vec3& p) { return b_in.sample(p); }, b_in.bounds(), h);
        bp = &resampled;
    }
    const Grid& b = *bp;
    // every brick where either field is stored or inside; elsewhere both are
    // +band (outside) and every operation gives +band
    std::unordered_set<IVec3, IVec3Hash> coords;
    for (const Grid* g : {&a, &b}) {
        for (const auto& [c, _] : g->bricks()) coords.insert(c);
        for (const auto& c : g->interior_bricks()) coords.insert(c);
    }
    auto value = [](const Grid& g, const IVec3& c, int idx) {
        auto it = g.bricks().find(c);
        if (it == g.bricks().end()) return g.is_interior(c) ? -kBand : kBand;
        return half_to_float(it->second->v[idx]);
    };
    Grid out = a;  // shares a's bricks; written bricks are copied on write
    std::vector<IVec3> touched;
    for (const auto& c : coords) {
        const bool b_here = b.allocated(c) || b.is_interior(c);
        if (!b_here && op != GridOp::Intersection) continue;  // b outside: union/difference leave a as it is
        std::array<std::uint16_t, kBrickVoxels> hv;
        for (int i = 0; i < kBrickVoxels; ++i) {
            const float va = value(a, c, i), vb = value(b, c, i);
            // polynomial smooth min/max with k = 2 voxels (values are in units of h):
            // cracks and slivers thinner than about one voxel (e.g. a sole resting on a
            // stone top) are filled with a small fillet instead of producing two
            // surface sheets in one cell
            constexpr float k = 2.0f;
            auto smin = [&](float x, float y) {
                const float hh = std::max(k - std::abs(x - y), 0.0f) / k;
                return std::min(x, y) - hh * hh * k * 0.25f;
            };
            float r = op == GridOp::Union ? smin(va, vb) : op == GridOp::Difference ? -smin(-va, vb) : -smin(-va, -vb);
            r = std::clamp(r, -kBand, kBand);
            if (r == 0.0f) r = kZeroNudge;
            hv[i] = float_to_half(r);
        }
        Brick& dst = out.mutable_brick(c);  // leaves the interior set if it was there
        dst.v = hv;
        out.touch_mesh(c);
        touched.push_back(c);
    }
    out.compact(touched);  // all -band -> interior again, all +band -> dropped
    return out;
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
    // Placement: mass point of the edge crossings (Surface Nets, Gibson 1998).
    // Measured on the M3 acceptance strokes: the QEF minimiser of Ju et al.
    // (truncated at sigma_i < 0.1 sigma_1, clamped to the cell) produced 294
    // self-intersections and 8 degenerate triangles, a QEF displacement
    // limited to 0.35 h still 7, the mass point 0 -- and an export must pass
    // the checks. Sharp features come from the exact kernel (booleans with
    // CAD solids run on the output mesh); see ADR 0017.
    bool vertex(int i, int j, int k, Vec3& out) {
        float v[2][2][2];
        for (int z = 0; z < 2; ++z)
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 2; ++x) v[z][y][x] = acc.at(i + x, j + y, k + z);
        static const int E[12][6] = {{0, 0, 0, 1, 0, 0}, {0, 1, 0, 1, 1, 0}, {0, 0, 1, 1, 0, 1}, {0, 1, 1, 1, 1, 1},
                                     {0, 0, 0, 0, 1, 0}, {1, 0, 0, 1, 1, 0}, {0, 0, 1, 0, 1, 1}, {1, 0, 1, 1, 1, 1},
                                     {0, 0, 0, 0, 0, 1}, {1, 0, 0, 1, 0, 1}, {0, 1, 0, 0, 1, 1}, {1, 1, 0, 1, 1, 1}};
        double mx = 0, my = 0, mz = 0;
        int n = 0;
        for (const auto& e : E) {
            const float a = v[e[2]][e[1]][e[0]], c = v[e[5]][e[4]][e[3]];
            if (!crossing(a, c)) continue;
            const double t = a / double(a - c);
            mx += e[0] + t * (e[3] - e[0]), my += e[1] + t * (e[4] - e[1]), mz += e[2] + t * (e[5] - e[2]);
            ++n;
        }
        if (!n) return false;
        out = Vec3{(i + mx / n) * h, (j + my / n) * h, (k + mz / n) * h};
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
    split_nonmanifold(out);  // sheets touching inside one cell
    return out;
}

std::vector<IVec3> chunks_of(const Grid& g) {
    std::unordered_set<IVec3, IVec3Hash> s;
    for (const auto& [b, _] : g.bricks()) s.insert(chunk_of_brick(b));
    return {s.begin(), s.end()};
}

// ------------------------------------------------------------------ ray casting
std::optional<double> raycast(const Grid& g, const Vec3& o, const Vec3& d, double t_max) {
    const Box3 b = g.bounds();
    if (b.empty()) return std::nullopt;
    // clip the ray to the bounds of the allocated bricks (slab test)
    double t0 = 0, t1 = t_max;
    for (int a = 0; a < 3; ++a) {
        if (std::abs(d[a]) < 1e-300) {
            if (o[a] < b.lo[a] || o[a] > b.hi[a]) return std::nullopt;
            continue;
        }
        double ta = (b.lo[a] - o[a]) / d[a], tb = (b.hi[a] - o[a]) / d[a];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta), t1 = std::min(t1, tb);
        if (t0 > t1) return std::nullopt;
    }
    const double h = g.h();
    double t = t0, f = g.sample(o + d * t);
    if (f < 0) return t;  // starts inside
    for (int i = 0; i < 100000 && t <= t1; ++i) {
        const double step = std::max(f, 0.5 * h);
        const double tn = t + step;
        const double fn = g.sample(o + d * tn);
        if (fn < 0) {  // bracket [t, tn]: bisect
            double a = t, c = tn;
            for (int k = 0; k < 30; ++k) {
                const double m = 0.5 * (a + c);
                (g.sample(o + d * m) < 0 ? c : a) = m;
            }
            return 0.5 * (a + c);
        }
        t = tn, f = fn;
    }
    return std::nullopt;
}

// ------------------------------------------------------------------ brushes
const char* brush_name(Brush b) {
    switch (b) {
        case Brush::Draw: return "draw";
        case Brush::Carve: return "carve";
        case Brush::Smooth: return "smooth";
        case Brush::Grab: return "grab";
        case Brush::Flatten: return "flatten";
        case Brush::Inflate: return "inflate";
        case Brush::Clay: return "clay";
        case Brush::Crease: return "crease";
        case Brush::Pinch: return "pinch";
        case Brush::Texture: return "texture";
    }
    return "draw";
}
const char* pattern_name(Pattern p) {
    switch (p) {
        case Pattern::Scales: return "scales";
        case Pattern::Chainmail: return "chainmail";
        case Pattern::Rivets: return "rivets";
        case Pattern::Stone: return "stone";
    }
    return "scales";
}
bool parse_pattern(const std::string& s, Pattern& out) {
    for (Pattern p : {Pattern::Scales, Pattern::Chainmail, Pattern::Rivets, Pattern::Stone})
        if (s == pattern_name(p)) return out = p, true;
    return false;
}
const std::vector<Brush>& all_brushes() {
    static const std::vector<Brush> v{Brush::Draw,    Brush::Carve, Brush::Inflate, Brush::Clay, Brush::Crease,
                                      Brush::Pinch,   Brush::Smooth, Brush::Grab,   Brush::Flatten, Brush::Texture};
    return v;
}
bool parse_brush(const std::string& s, Brush& out) {
    for (Brush b : all_brushes())
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
// Unchanged voxels are bit-identical floats (both copies came from the same
// halfs), so changes are found without converting.
std::vector<IVec3> store_block(Grid& g, const Block& B, const Block& before) {
    std::vector<IVec3> changed;
    for (int bz = 1; bz < B.nb.z - 1; ++bz)
        for (int by = 1; by < B.nb.y - 1; ++by)
            for (int bx = 1; bx < B.nb.x - 1; ++bx) {
                bool diff = false;
                for (int z = 0; z < kBrick && !diff; ++z)
                    for (int y = 0; y < kBrick && !diff; ++y)
                        for (int x = 0; x < kBrick && !diff; ++x) {
                            const int X = bx * kBrick + x, Y = by * kBrick + y, Z = bz * kBrick + z;
                            diff = std::clamp(B.at(X, Y, Z), -kBand, kBand) != std::clamp(before.at(X, Y, Z), -kBand, kBand);
                        }
                if (!diff) continue;
                std::array<std::uint16_t, kBrickVoxels> hv;
                bool real = false;      // differences below half precision vanish on conversion
                bool relevant = false;  // a value the surface depends on changed (|f| < 2.5 h)
                for (int z = 0; z < kBrick; ++z)
                    for (int y = 0; y < kBrick; ++y)
                        for (int x = 0; x < kBrick; ++x) {
                            const int X = bx * kBrick + x, Y = by * kBrick + y, Z = bz * kBrick + z;
                            float nvf = std::clamp(B.at(X, Y, Z), -kBand, kBand);
                            if (nvf == 0.0f) nvf = kZeroNudge;
                            const std::uint16_t nv = float_to_half(nvf);
                            hv[Brick::index(x, y, z)] = nv;
                            const float ov = std::clamp(before.at(X, Y, Z), -kBand, kBand);
                            if (nv != float_to_half(ov)) {
                                real = true;
                                relevant = relevant || std::min(std::abs(ov), std::abs(half_to_float(nv))) < 2.5f;
                            }
                        }
                if (!real) continue;
                IVec3 bc{B.b0.x + bx, B.b0.y + by, B.b0.z + bz};
                Brick& dst = g.mutable_brick(bc);
                dst.v = hv;
                if (relevant) dst.stamp = g_stamp.fetch_add(1);
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
    if (lo.x > hi.x || lo.y > hi.y || lo.z > hi.z) return;
    constexpr float kNear = 1.5f;
    const std::size_t nz = std::size_t(hi.z - lo.z + 1);
    {
        const Block src = B;
        parallel_for(nz, [&](std::size_t iz) {  // each voxel reads only src: deterministic
            const int z = lo.z + int(iz);
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
        });
    }
    static const float W[3] = {1.0f, 1.41421356f, 1.73205081f};
    for (int pass = 0; pass < 2; ++pass) {
        const Block src = B;
        parallel_for(nz, [&](std::size_t iz) {
            const int z = lo.z + int(iz);
            for (int y = lo.y; y <= hi.y; ++y)
                for (int x = lo.x; x <= hi.x; ++x) {
                    const float f = src.at(x, y, z);
                    if (std::abs(f) < kNear) continue;
                    // far from the surface (clamped, face neighbours clamped): nothing to rebuild;
                    // a 1-Lipschitz field cannot reach |f| < kBand - sqrt 2 diagonally from here
                    if (std::abs(f) >= kBand && std::abs(src.at(x + 1, y, z)) >= kBand && std::abs(src.at(x - 1, y, z)) >= kBand &&
                        std::abs(src.at(x, y + 1, z)) >= kBand && std::abs(src.at(x, y - 1, z)) >= kBand &&
                        std::abs(src.at(x, y, z + 1)) >= kBand && std::abs(src.at(x, y, z - 1)) >= kBand)
                        continue;
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
        });
    }
}
}  // namespace

namespace {
inline double phash(int i, int j, int salt) {
    std::uint32_t x = std::uint32_t(i) * 73856093u ^ std::uint32_t(j) * 19349663u ^ std::uint32_t(salt) * 83492791u;
    x ^= x >> 16, x *= 0x7feb352du, x ^= x >> 15, x *= 0x846ca68bu, x ^= x >> 16;
    return (x & 0xffffff) / double(0x1000000);
}
// Relief height in [0, 1] at tangent-plane coordinates (u, v), cell size s.
double pattern_height(Pattern pt, double u, double v, double s) {
    const double x = u / s, y = v / s;
    switch (pt) {
        case Pattern::Rivets: {  // hemispherical heads on a square grid
            const double dx = x - std::round(x), dy = y - std::round(y);
            const double r = std::hypot(dx, dy) / 0.28;
            return r < 1 ? std::sqrt(1 - r * r) : 0.0;
        }
        case Pattern::Scales: {  // overlapping rows (offset by half a cell), each scale a rounded tongue
            double best = 0;
            const int j0 = int(std::floor(y));
            for (int j = j0 - 1; j <= j0 + 1; ++j) {
                const double off = (j & 1) ? 0.5 : 0.0;
                const int i0 = int(std::floor(x - off));
                for (int i = i0 - 1; i <= i0 + 1; ++i) {
                    const double cx = i + off + 0.5, cy = j + 0.5;
                    const double r = std::hypot((x - cx) / 0.62, (y - cy - 0.15) / 0.75);
                    if (r < 1) best = std::max(best, (1 - r) * (0.55 + 0.45 * (cy - y + 0.5)));  // thicker at the free edge
                }
            }
            return std::clamp(best * 1.6, 0.0, 1.0);
        }
        case Pattern::Chainmail: {  // rings on an offset lattice
            double best = 0;
            const int j0 = int(std::floor(y * 2));
            for (int j = j0 - 1; j <= j0 + 1; ++j) {
                const double off = (j & 1) ? 0.5 : 0.0;
                const int i0 = int(std::floor(x - off));
                for (int i = i0 - 1; i <= i0 + 1; ++i) {
                    const double cx = i + off + 0.5, cy = 0.5 * j + 0.25;
                    const double ring = std::abs(std::hypot(x - cx, y - cy) - 0.36) / 0.11;
                    if (ring < 1) best = std::max(best, std::sqrt(1 - ring * ring));
                }
            }
            return best;
        }
        case Pattern::Stone: {  // Voronoi stones with joints
            const int ci = int(std::floor(x)), cj = int(std::floor(y));
            double d1 = 1e9, d2 = 1e9;
            int bi = 0, bj = 0;
            for (int j = cj - 1; j <= cj + 1; ++j)
                for (int i = ci - 1; i <= ci + 1; ++i) {
                    const double px = i + 0.15 + 0.7 * phash(i, j, 1), py = j + 0.15 + 0.7 * phash(i, j, 2);
                    const double d = std::hypot(x - px, y - py);
                    if (d < d1) d2 = d1, d1 = d, bi = i, bj = j;
                    else if (d < d2) d2 = d;
                }
            const double edge = 0.5 * (d2 - d1);
            const double t = std::clamp(edge / 0.12, 0.0, 1.0);
            return t * t * (3 - 2 * t) * (0.7 + 0.3 * phash(bi, bj, 3));
        }
    }
    return 0;
}
}  // namespace

StrokeApplier::StrokeApplier(Grid& g, Brush brush, double radius, double strength, StrokeOptions opt)
    : g_(g), brush_(brush), R_(radius), st_(std::clamp(strength, 0.0, 1.0)), spacing_(radius / 4), opt_(opt) {
    if (!(radius > 0)) throw std::invalid_argument("sculpt radius must be > 0");
    if (!(opt_.falloff > 0)) throw std::invalid_argument("sculpt falloff must be > 0");
    if (opt_.symmetry < -1 || opt_.symmetry > 2) throw std::invalid_argument("sculpt symmetry axis must be x, y or z");
}

Vec3 StrokeApplier::mirror(const Vec3& p) const {
    Vec3 q = p;
    if (opt_.symmetry == 0) q.x = 2 * opt_.sym_at - p.x;
    if (opt_.symmetry == 1) q.y = 2 * opt_.sym_at - p.y;
    if (opt_.symmetry == 2) q.z = 2 * opt_.sym_at - p.z;
    return q;
}

void StrokeApplier::add(const Vec3& p) {
    if (finished_) return;
    if (!have_first_) {
        have_first_ = true;
        last_point_ = last_dab_ = p;
        // flatten: plane through the first point, normal = outward field gradient there
        plane_n_ = normalized(g_.gradient(p));
        plane_d_ = dot(plane_n_, p);
        if (opt_.symmetry >= 0) {
            const Vec3 pm = mirror(p);
            plane_n_m_ = normalized(g_.gradient(pm));
            plane_d_m_ = dot(plane_n_m_, pm);
        }
        if (brush_ != Brush::Grab) dab(p, {});
        return;
    }
    // resample the polyline into dabs `spacing` apart
    const Vec3 a = last_point_, b = p;
    const double L = norm(b - a);
    double t = spacing_ - carry_;
    while (t <= L) {
        const Vec3 c = a + (b - a) * (t / L);
        if (brush_ == Brush::Grab) dab(last_dab_, c - last_dab_);
        else dab(c, {});
        last_dab_ = c;
        t += spacing_;
    }
    carry_ = L - (t - spacing_);
    last_point_ = p;
}

void StrokeApplier::finish() {
    if (finished_) return;
    finished_ = true;
    if (brush_ == Brush::Grab && have_first_ && !(last_dab_ == last_point_)) dab(last_dab_, last_point_ - last_dab_);
    std::vector<IVec3> all(all_touched_.begin(), all_touched_.end());
    g_.compact(all);
}

std::vector<IVec3> StrokeApplier::take_touched() {
    std::vector<IVec3> out(touched_.begin(), touched_.end());
    touched_.clear();
    return out;
}

void StrokeApplier::dab(const Vec3& cc, const Vec3& grab) {
    ++dabs_;
    dab_one(cc, grab, plane_n_, plane_d_);
    if (opt_.symmetry >= 0) {
        Vec3 gm = grab;
        if (opt_.symmetry == 0) gm.x = -gm.x;
        if (opt_.symmetry == 1) gm.y = -gm.y;
        if (opt_.symmetry == 2) gm.z = -gm.z;
        const Vec3 cm = mirror(cc);
        // on the mirror plane both halves would hit the same spot twice
        if (norm(cm - cc) > 1e-9 * std::max(1.0, R_)) dab_one(cm, gm, plane_n_m_, plane_d_m_);
    }
}

void StrokeApplier::dab_one(const Vec3& cc, const Vec3& grab, const Vec3& pn, double pd) {
    const double h = g_.h(), R = R_, st = st_;
    const double sign = opt_.invert ? -1.0 : 1.0;
    // displacement height per dab (see Stroke): a = 0.2 R s
    const double a = 0.2 * R * st;
    const double tex_amp = 0.3 * opt_.pattern_size * st;  // texture relief height (mm)
    const double disp = (brush_ == Brush::Grab)                                   ? norm(grab)
                        : (brush_ == Brush::Smooth || brush_ == Brush::Flatten)   ? 0.0
                        : brush_ == Brush::Texture                                ? tex_amp
                                                                                  : a;
    const double reach = R + (kBand + 1) * h + disp;
    Block B = load_block(g_, cc - Vec3{reach, reach, reach}, cc + Vec3{reach, reach, reach});
    const Block before = B;
    // mean outward normal at the dab point (direction of draw/clay/crease/pinch)
    Vec3 n0 = normalized(g_.gradient(cc));
    if (norm(n0) == 0) n0 = {0, 0, 1};
    // Grab: semi-Lagrangian sub-steps of at most R/4 (fold limit 0.47 R, see Stroke)
    int sub = brush_ == Brush::Grab ? std::max(1, int(std::ceil(disp / (0.25 * R)))) : 1;
    if (brush_ == Brush::Smooth) sub = 1 + int(std::lround(4 * st));  // MBO iterations
    const Vec3 origin = Vec3{double(B.b0.x), double(B.b0.y), double(B.b0.z)} * (kBrick * h);
    const double fexp = brush_ == Brush::Crease ? 3.0 * opt_.falloff : opt_.falloff;
    for (int ss = 0; ss < sub; ++ss) {
        const Block src = B;
        // voxels inside the dab's sphere bounding box (block coordinates)
        const Vec3 cv = (cc - origin) / h;
        const int rv = int(std::ceil(R / h)) + 1;
        const int x0 = std::max(1, int(cv.x) - rv), x1 = std::min(B.nx - 2, int(cv.x) + rv);
        const int y0 = std::max(1, int(cv.y) - rv), y1 = std::min(B.ny - 2, int(cv.y) + rv);
        const int z0 = std::max(1, int(cv.z) - rv), z1 = std::min(B.nz - 2, int(cv.z) + rv);
        parallel_for(std::size_t(std::max(0, z1 - z0 + 1)), [&](std::size_t iz) {
            const int z = z0 + int(iz);
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const Vec3 p = origin + Vec3{double(x), double(y), double(z)} * h;
                    const double q = norm(p - cc) / R;
                    if (q >= 1.0) continue;
                    double w = wendland(q);
                    if (fexp != 1.0) w = std::pow(w, fexp);
                    float& f = B.at(x, y, z);
                    const float f0 = src.at(x, y, z);
                    auto sample_shift = [&](const Vec3& d_mm) {  // f(x - d), semi-Lagrangian
                        const Vec3 d = d_mm / h;
                        return float(src.sample(x - d.x, y - d.y, z - d.z));
                    };
                    switch (brush_) {
                        case Brush::Draw:
                            f = sample_shift(n0 * (sign * a * w));
                            break;
                        case Brush::Carve:
                            f = sample_shift(n0 * (-sign * a * w));
                            break;
                        case Brush::Inflate: {
                            // along the local normal of the field (offsets the surface)
                            const double gx = 0.5 * (src.at(x + 1, y, z) - src.at(x - 1, y, z));
                            const double gy = 0.5 * (src.at(x, y + 1, z) - src.at(x, y - 1, z));
                            const double gz = 0.5 * (src.at(x, y, z + 1) - src.at(x, y, z - 1));
                            const double gn = std::sqrt(gx * gx + gy * gy + gz * gz);
                            if (gn < 1e-6 || std::abs(f0) >= kBand) break;
                            f = sample_shift(Vec3{gx, gy, gz} * (sign * a * w / gn));
                            break;
                        }
                        case Brush::Clay: {
                            // fill up to the plane a above the dab point: blend towards the
                            // union min(f, f_plane) (inverted: cut, max(f, -f_plane')). A min/max
                            // of distance fields stays 1-Lipschitz, so the band stays consistent
                            // (no jumps the contouring would reject).
                            const double fp = std::clamp((dot(n0, p - cc) - sign * a) / h, -double(kBand), double(kBand));
                            const double target = opt_.invert ? std::max<double>(f0, fp) : std::min<double>(f0, fp);
                            f = float(f0 + st * w * (target - f0));
                            break;
                        }
                        case Brush::Crease: {
                            // groove: carve along n0 plus a tangential pull towards the centre
                            Vec3 t = p - cc;
                            t = t - n0 * dot(t, n0);
                            f = sample_shift(n0 * (-sign * 0.6 * a * w) - t * (0.35 * st * w));
                            break;
                        }
                        case Brush::Pinch: {
                            // material moves towards the stroke: sample further out
                            Vec3 t = p - cc;
                            t = t - n0 * dot(t, n0);
                            f = sample_shift(t * (-sign * 0.4 * st * w));
                            break;
                        }
                        case Brush::Smooth: {
                            // MBO-style (Merriman, Bence & Osher 1992): diffuse the clamped
                            // indicator c = clamp(f/h, -1, 1), which moves the zero level by
                            // mean curvature. Averaging f itself would pull the interior
                            // medial axis (ridge of the SDF) into convex corners and sharpen them.
                            if (std::abs(f0) >= 1.5f) break;
                            auto c = [](float v) { return std::clamp(v, -1.0f, 1.0f); };
                            double avg = (c(src.at(x + 1, y, z)) + c(src.at(x - 1, y, z)) + c(src.at(x, y + 1, z)) +
                                          c(src.at(x, y - 1, z)) + c(src.at(x, y, z + 1)) + c(src.at(x, y, z - 1))) / 6.0;
                            f = float(f0 + 0.5 * w * (avg - c(f0)));
                            break;
                        }
                        case Brush::Grab: {  // semi-Lagrangian: f(x) <- f(x - w delta)
                            f = sample_shift(grab * (w / sub));
                            break;
                        }
                        case Brush::Texture: {
                            // pattern in the plane across the dominant axis of n0 (world-fixed,
                            // so neighbouring dabs continue the same pattern seamlessly)
                            const int ax = std::abs(n0.x) > std::abs(n0.y) ? (std::abs(n0.x) > std::abs(n0.z) ? 0 : 2)
                                                                            : (std::abs(n0.y) > std::abs(n0.z) ? 1 : 2);
                            const double u = ax == 0 ? p.y : p.x, v = ax == 2 ? p.y : p.z;
                            const double ph = pattern_height(opt_.pattern, u, v, opt_.pattern_size);
                            f = sample_shift(n0 * (sign * tex_amp * w * ph));
                            break;
                        }
                        case Brush::Flatten: {
                            if (std::abs(f0) >= kBand) break;
                            const double fp = (dot(pn, p) - pd) / h;
                            f = float(f0 + st * w * (fp - f0));
                            break;
                        }
                    }
                }
        });
    }
    {
        // only where the brush changed values (sphere box + displacement + the 3-voxel stencil reach)
        const int rr = int(std::ceil((R + disp) / h)) + 3;
        const Vec3 cb = (cc - origin) / h;
        reinit(B, {int(cb.x) - rr, int(cb.y) - rr, int(cb.z) - rr}, {int(cb.x) + rr, int(cb.y) + rr, int(cb.z) + rr});
    }
    for (const auto& b : store_block(g_, B, before)) touched_.insert(b), all_touched_.insert(b);
}

std::vector<IVec3> apply_stroke(Grid& g, const Stroke& s) {
    if (s.path.empty()) return {};
    StrokeApplier a(g, s.brush, s.radius, s.strength, s.opt);
    for (const auto& p : s.path) a.add(p);
    a.finish();
    return a.take_touched();
}

// ------------------------------------------------------------------ chunk meshes
namespace {
// cell coordinates packed into 63 bits (21 bits per axis, offset 2^20):
// +-1.0e6 cells = +-52 m at h = 0.05 mm
inline std::uint64_t pack_cell(const IVec3& c) {
    const std::uint64_t o = 1u << 20;
    return ((std::uint64_t(c.x + o) & 0x1fffff) << 42) | ((std::uint64_t(c.y + o) & 0x1fffff) << 21) |
           (std::uint64_t(c.z + o) & 0x1fffff);
}
struct ChunkCache {
    std::mutex mu;
    std::unordered_map<std::uint64_t, std::pair<std::uint64_t, std::shared_ptr<const ChunkMesh>>> map;
    std::uint64_t tick = 0;
    std::size_t bytes = 0;
    static constexpr std::size_t kMax = 40000;
    // dense clay: ~10k triangles (~1 MB) per chunk, so 40000 chunks could reach
    // tens of GB; the byte budget bounds it (meshes still shown stay alive)
    static constexpr std::size_t kBudgetBytes = std::size_t(512) << 20;  // 512 MiB
    static std::size_t size_of(const ChunkMesh& c) {
        return sizeof(ChunkMesh) + c.mesh.V.capacity() * sizeof(Vec3) + c.mesh.F.capacity() * sizeof(Tri) +
               c.cell.capacity() * 8 + c.normal.capacity() * sizeof(c.normal[0]);
    }
    void insert(std::uint64_t key, std::shared_ptr<const ChunkMesh> cm) {  // mu held
        if (auto it = map.find(key); it != map.end()) bytes -= size_of(*it->second.second);
        bytes += size_of(*cm);
        map[key] = {++tick, std::move(cm)};
        if (map.size() > kMax || bytes > kBudgetBytes) {  // drop the oldest quarter
            std::vector<std::pair<std::uint64_t, std::uint64_t>> ages;
            for (const auto& [k, v] : map) ages.push_back({v.first, k});
            std::nth_element(ages.begin(), ages.begin() + ages.size() / 4, ages.end());
            for (std::size_t i = 0; i < ages.size() / 4; ++i) {
                auto it = map.find(ages[i].second);
                bytes -= size_of(*it->second.second);
                map.erase(it);
            }
        }
    }
};
ChunkCache& chunk_cache() {
    static ChunkCache c;
    return c;
}
// Values around one brick: the 3x3x3 neighbour bricks resolved once.
struct Neighbourhood {
    const Brick* b[27];
    float implied[27];
    void load(const Grid& g, const IVec3& c) {
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int k = (dz + 1) * 9 + (dy + 1) * 3 + (dx + 1);
                    const IVec3 n{c.x + dx, c.y + dy, c.z + dz};
                    auto it = g.bricks().find(n);
                    b[k] = it == g.bricks().end() ? nullptr : it->second.get();
                    implied[k] = g.is_interior(n) ? -kBand : kBand;
                }
    }
    // voxel (x, y, z) relative to the centre brick's first voxel, -8 .. 15
    float at(int x, int y, int z) const {
        const int bx = x < 0 ? -1 : x >= kBrick, by = y < 0 ? -1 : y >= kBrick, bz = z < 0 ? -1 : z >= kBrick;
        const int k = (bz + 1) * 9 + (by + 1) * 3 + (bx + 1);
        if (!b[k]) return implied[k];
        return half_table().t[b[k]->v[Brick::index(x - bx * kBrick, y - by * kBrick, z - bz * kBrick)]];
    }
};

std::shared_ptr<const ChunkMesh> build_chunk(const Grid& g, const IVec3& chunk, std::uint64_t key) {
    // Same topology and vertex arithmetic as contour()/CellSolver (bit-identical
    // vertices across chunk borders), but: only the chunk's own bricks, values
    // through a neighbour table, cell vertices in a dense array.
    auto cm = std::make_shared<ChunkMesh>();
    cm->key = key, cm->chunk = chunk;
    constexpr int kCells = kChunkBricks * kBrick;  // cells per axis owned by the chunk
    constexpr int kSpan = kCells + 1;              // cells referenced: lo-1 .. lo+kCells-1
    const IVec3 lo{chunk.x * kCells, chunk.y * kCells, chunk.z * kCells};
    std::vector<std::int32_t> vid(std::size_t(kSpan) * kSpan * kSpan, -2);  // -2 unknown, -1 none
    const double h = g.h();
    Mesh& out = cm->mesh;
    std::vector<std::uint64_t>& cells = cm->cell;
    static const int E[12][6] = {{0, 0, 0, 1, 0, 0}, {0, 1, 0, 1, 1, 0}, {0, 0, 1, 1, 0, 1}, {0, 1, 1, 1, 1, 1},
                                 {0, 0, 0, 0, 1, 0}, {1, 0, 0, 1, 1, 0}, {0, 0, 1, 0, 1, 1}, {1, 0, 1, 1, 1, 1},
                                 {0, 0, 0, 0, 0, 1}, {1, 0, 0, 1, 0, 1}, {0, 1, 0, 0, 1, 1}, {1, 1, 0, 1, 1, 1}};
    Neighbourhood nb;
    for (int cz = 0; cz < kChunkBricks; ++cz)
        for (int cy = 0; cy < kChunkBricks; ++cy)
            for (int cx = 0; cx < kChunkBricks; ++cx) {
                const IVec3 bc{chunk.x * kChunkBricks + cx, chunk.y * kChunkBricks + cy, chunk.z * kChunkBricks + cz};
                auto bit = g.bricks().find(bc);
                if (bit == g.bricks().end()) continue;
                nb.load(g, bc);
                const IVec3 o{bc.x * kBrick, bc.y * kBrick, bc.z * kBrick};  // first voxel of the brick
                // vertex of cell (global i, j, k); cells used here lie within one voxel of this brick
                auto vert = [&](int i, int j, int k) -> std::int64_t {
                    const std::size_t di = std::size_t(i - lo.x + 1), dj = std::size_t(j - lo.y + 1), dk = std::size_t(k - lo.z + 1);
                    std::int32_t& slot = vid[(dk * kSpan + dj) * kSpan + di];
                    if (slot != -2) return slot;
                    float v[2][2][2];
                    for (int z = 0; z < 2; ++z)
                        for (int y = 0; y < 2; ++y)
                            for (int x = 0; x < 2; ++x) v[z][y][x] = nb.at(i - o.x + x, j - o.y + y, k - o.z + z);
                    double mx = 0, my = 0, mz = 0;
                    int n = 0;
                    for (const auto& e : E) {
                        const float a = v[e[2]][e[1]][e[0]], c = v[e[5]][e[4]][e[3]];
                        if (!crossing(a, c)) continue;
                        const double t = a / double(a - c);
                        mx += e[0] + t * (e[3] - e[0]), my += e[1] + t * (e[4] - e[1]), mz += e[2] + t * (e[5] - e[2]);
                        ++n;
                    }
                    if (!n) return slot = -1;
                    out.V.push_back(Vec3{(i + mx / n) * h, (j + my / n) * h, (k + mz / n) * h});
                    cells.push_back(pack_cell({i, j, k}));
                    // normal: central-difference gradients at the 8 corners, trilinear at the vertex
                    {
                        const double u = mx / n, vv = my / n, ww = mz / n;
                        Vec3 gsum{};
                        for (int c = 0; c < 8; ++c) {
                            const int cx = c & 1, cy = (c >> 1) & 1, cz = c >> 2;
                            const int X = i - o.x + cx, Y = j - o.y + cy, Z = k - o.z + cz;
                            const Vec3 gc{nb.at(X + 1, Y, Z) - nb.at(X - 1, Y, Z), nb.at(X, Y + 1, Z) - nb.at(X, Y - 1, Z),
                                          nb.at(X, Y, Z + 1) - nb.at(X, Y, Z - 1)};
                            gsum += gc * ((cx ? u : 1 - u) * (cy ? vv : 1 - vv) * (cz ? ww : 1 - ww));
                        }
                        const double gl = norm(gsum);
                        const Vec3 nv = gl > 0 ? gsum / gl : Vec3{0, 0, 1};
                        cm->normal.push_back({float(nv.x), float(nv.y), float(nv.z)});
                    }
                    return slot = std::int32_t(out.V.size() - 1);
                };
                const auto& brick = bit->second;
                for (int z = 0; z < kBrick; ++z)
                    for (int y = 0; y < kBrick; ++y)
                        for (int x = 0; x < kBrick; ++x) {
                            const int i = o.x + x, j = o.y + y, k = o.z + z;
                            const float f0 = half_table().t[brick->v[Brick::index(x, y, z)]];
                            for (int a = 0; a < 3; ++a) {
                                const float f1 = nb.at(x + (a == 0), y + (a == 1), z + (a == 2));
                                if (!crossing(f0, f1)) continue;
                                const int u = (a + 1) % 3, w = (a + 2) % 3;
                                auto cell = [&](int du, int dw) {
                                    int d[3] = {0, 0, 0};
                                    d[u] = du, d[w] = dw;
                                    return vert(i + d[0], j + d[1], k + d[2]);
                                };
                                std::int64_t q[4] = {cell(-1, -1), cell(0, -1), cell(0, 0), cell(-1, 0)};
                                if (q[0] < 0 || q[1] < 0 || q[2] < 0 || q[3] < 0) continue;
                                if (f0 >= 0) std::swap(q[1], q[3]);
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
    return cm;
}
}  // namespace

void clear_chunk_cache() {
    auto& cc = chunk_cache();
    std::lock_guard lk(cc.mu);
    cc.map.clear();
    cc.bytes = 0;
}

std::pair<std::size_t, std::size_t> chunk_cache_stats() {
    auto& cc = chunk_cache();
    std::lock_guard lk(cc.mu);
    return {cc.map.size(), cc.bytes};
}

std::shared_ptr<const ChunkMesh> chunk_mesh(const Grid& g, const IVec3& chunk) {
    const std::uint64_t key = g.chunk_key(chunk);
    auto& cc = chunk_cache();
    {
        std::lock_guard lk(cc.mu);
        auto it = cc.map.find(key);
        if (it != cc.map.end()) {
            it->second.first = ++cc.tick;
            return it->second.second;
        }
    }
    auto cm = build_chunk(g, chunk, key);
    std::lock_guard lk(cc.mu);
    cc.insert(key, cm);
    return cm;
}

std::vector<std::shared_ptr<const ChunkMesh>> remesh_chunks(const Grid& g, const std::vector<IVec3>& chunks,
                                                            std::size_t* built) {
    std::vector<std::shared_ptr<const ChunkMesh>> out(chunks.size());
    std::atomic<std::size_t> n{0};
    parallel_for(chunks.size(), [&](std::size_t i) {
        const std::uint64_t key = g.chunk_key(chunks[i]);
        {
            auto& cc = chunk_cache();
            std::lock_guard lk(cc.mu);
            auto it = cc.map.find(key);
            if (it != cc.map.end()) {
                it->second.first = ++cc.tick;
                out[i] = it->second.second;
                return;
            }
        }
        out[i] = chunk_mesh(g, chunks[i]);
        ++n;
    });
    if (built) *built += n;
    return out;
}

std::vector<std::shared_ptr<const ChunkMesh>> chunk_meshes(const Grid& g) {
    auto chunks = chunks_of(g);
    std::sort(chunks.begin(), chunks.end(), [](const IVec3& a, const IVec3& b) {
        return std::tie(a.z, a.y, a.x) < std::tie(b.z, b.y, b.x);
    });
    std::vector<std::shared_ptr<const ChunkMesh>> out(chunks.size());
    parallel_for(chunks.size(), [&](std::size_t i) { out[i] = chunk_mesh(g, chunks[i]); });
    return out;
}

Mesh weld(const std::vector<std::shared_ptr<const ChunkMesh>>& parts) {
    std::size_t nv = 0;
    for (const auto& p : parts) nv += p->mesh.V.size();
    std::vector<std::uint64_t> keys;
    std::vector<std::uint32_t> idx;
    keys.reserve(nv), idx.reserve(nv);
    std::vector<std::size_t> base;
    for (const auto& p : parts) {
        base.push_back(keys.size());
        for (std::size_t i = 0; i < p->mesh.V.size(); ++i) keys.push_back(p->cell[i]), idx.push_back(std::uint32_t(idx.size()));
    }
    radix_sort_pairs(keys, idx, 63);
    std::vector<std::uint32_t> remap(nv);
    Mesh out;
    std::vector<Vec3> flat;
    flat.reserve(nv);
    for (const auto& p : parts) flat.insert(flat.end(), p->mesh.V.begin(), p->mesh.V.end());
    for (std::size_t i = 0; i < nv; ++i) {
        if (i == 0 || keys[i] != keys[i - 1]) out.V.push_back(flat[idx[i]]);
        remap[idx[i]] = std::uint32_t(out.V.size() - 1);
    }
    for (std::size_t pi = 0; pi < parts.size(); ++pi)
        for (const auto& t : parts[pi]->mesh.F)
            out.F.push_back({remap[base[pi] + t[0]], remap[base[pi] + t[1]], remap[base[pi] + t[2]]});
    split_nonmanifold(out);  // sheets touching inside one cell (features thinner than ~2h)
    return out;
}

}  // namespace anvil::sdf
