// SDF bricks, Dual Contouring and brushes (M3).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <tuple>

#include "anvil/sdf.hpp"
#include "anvil/validate.hpp"

using namespace anvil;
using namespace anvil::sdf;

namespace {
Grid sphere_grid(double r, double h, Vec3 c = {}) {
    Box3 b;
    b.add(c - Vec3{r, r, r});
    b.add(c + Vec3{r, r, r});
    return from_function([=](const Vec3& p) { return norm(p - c) - r; }, b, h);
}
double box_sdf(const Vec3& p, const Vec3& half) {  // exact box SDF (Quilez)
    Vec3 q{std::abs(p.x) - half.x, std::abs(p.y) - half.y, std::abs(p.z) - half.z};
    Vec3 qp{std::max(q.x, 0.0), std::max(q.y, 0.0), std::max(q.z, 0.0)};
    return norm(qp) + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0);
}
}  // namespace

TEST_CASE("binary16 conversion is correctly rounded and round-trips", "[sdf]") {
    for (float f : {0.0f, 1.0f, -2.5f, 3.0f, 0.333333f, -0.0009765625f, 65504.0f})
        CHECK(std::abs(half_to_float(float_to_half(f)) - f) <= std::abs(f) * 9.8e-4f + 6e-8f);  // 2^-10 relative
    CHECK(half_to_float(float_to_half(1.0f + 1.0f / 4096)) == 1.0f);  // below half an ulp: rounds down
    CHECK(half_to_float(float_to_half(1.0f + 3.0f / 2048)) == 1.0f + 2.0f / 1024);  // tie -> even
    // f/h at the band edge keeps ~2^-9 h resolution (ADR 0008)
    CHECK(std::abs(half_to_float(float_to_half(2.9993f)) - 2.9993f) < 1.0f / 512);
}

TEST_CASE("sphere: DC volume converges as O(h^2), surface is closed and manifold", "[sdf]") {
    const double r = 5.0, V = 4.0 / 3.0 * std::numbers::pi * r * r * r;
    double prev_err = 1e9;
    for (double h : {0.4, 0.2, 0.1}) {
        Grid g = sphere_grid(r, h, {0.3, -0.2, 0.1});
        Mesh m = dual_contour(g);
        auto rep = validate(m, {.self_intersections = false});
        INFO(rep.summary());
        CHECK(rep.ok);
        double err = std::abs(m.signed_volume() - V) / V;
        INFO("h=" << h << " rel. volume error " << err);
        CHECK(err < 0.6 * h * h / r);   // chord error kappa h^2 / 8 on 3 / r of the volume ~ 0.375 h^2 / r
        CHECK(err < prev_err);
        prev_err = err;
        // every vertex lies on the sphere within the voxel-scale tolerance
        double maxdev = 0;
        for (const auto& v : m.V) maxdev = std::max(maxdev, std::abs(norm(v - Vec3{0.3, -0.2, 0.1}) - r));
        CHECK(maxdev < 0.1 * h);
    }
}

TEST_CASE("mesh -> SDF -> DC keeps sharp box edges (QEF)", "[sdf]") {
    // a 6 x 4 x 3 mm box as a triangle mesh, offset from the lattice
    const Vec3 half{3.0, 2.0, 1.5}, c{0.13, 0.07, 0.21};
    Grid analytic = from_function([&](const Vec3& p) { return box_sdf(p - c, half); },
                                  Box3{c - half, c + half}, 0.1);
    Mesh dc = dual_contour(analytic);
    CHECK(validate(dc, {.self_intersections = false}).ok);
    CHECK(dc.signed_volume() == Catch::Approx(72.0).epsilon(2e-3));
    // the same box through the mesh path (pseudonormal signs)
    Mesh box;
    for (int i = 0; i < 8; ++i)
        box.V.push_back(c + Vec3{(i & 1 ? 1 : -1) * half.x, (i & 2 ? 1 : -1) * half.y, (i & 4 ? 1 : -1) * half.z});
    box.F = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
             {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
    REQUIRE(box.signed_volume() == Catch::Approx(72.0));
    Grid fm = from_mesh(box, 0.1);
    for (const auto& p : {c, c + Vec3{2.9, 0, 0}, c + Vec3{0, 0, 1.6}, c + Vec3{4, 3, 2}})
        CHECK(fm.sample(p) == Catch::Approx(std::clamp(box_sdf(p - c, half), -0.3, 0.3)).margin(1e-3));  // band-clamped
    CHECK(fm.sample(c + Vec3{2.95, 0.1, 0.2}) == Catch::Approx(-0.05).margin(1e-3));
    Mesh dm = dual_contour(fm);
    CHECK(validate(dm, {.self_intersections = false}).ok);
    CHECK(dm.signed_volume() == Catch::Approx(72.0).epsilon(2e-3));
}

TEST_CASE("brushes: draw adds, carve removes, smooth rounds, grab moves; snapshots stay intact", "[sdf]") {
    const double h = 0.1, r = 4.0;
    Grid g = sphere_grid(r, h);
    const Grid snapshot = g;  // undo state
    const double V0 = dual_contour(g).signed_volume();

    Stroke s;
    s.radius = 1.5, s.strength = 1.0;
    s.path = {{0, -1, r}, {0, 1, r}};  // across the north pole
    s.brush = Brush::Draw;
    auto changed = apply_stroke(g, s);
    CHECK(!changed.empty());
    const double V1 = dual_contour(g).signed_volume();
    CHECK(V1 > V0 + 0.05);
    CHECK(g.sample({0, 0, r + 0.05}) < 0);                 // the pole moved outwards
    CHECK(snapshot.sample({0, 0, r + 0.05}) > 0);          // ... but not in the snapshot (copy on write)
    CHECK(dual_contour(snapshot).signed_volume() == Catch::Approx(V0));
    CHECK(validate(dual_contour(g), {.self_intersections = false}).ok);

    s.brush = Brush::Carve;
    apply_stroke(g, s);
    apply_stroke(g, s);
    CHECK(dual_contour(g).signed_volume() < V1 - 0.05);

    // smoothing a box rounds its edges: volume decreases, vertices leave the corner
    const Vec3 half{2, 2, 2};
    Grid b = from_function([&](const Vec3& p) { return box_sdf(p, half); }, Box3{-half, half}, h);
    const double Vb = dual_contour(b).signed_volume();
    Stroke sm{Brush::Smooth, {{2, 2, 2}}, 1.0, 1.0};
    for (int i = 0; i < 5; ++i) apply_stroke(b, sm);
    CHECK(dual_contour(b).signed_volume() < Vb - 1e-3);
    CHECK(b.sample({1.95, 1.95, 1.95}) > 0);  // the corner is rounded away

    // grab drags the pole up by ~ the path length
    Grid gg = sphere_grid(r, h);
    Stroke gr{Brush::Grab, {{0, 0, r}, {0, 0, r + 0.6}}, 1.5, 1.0};
    apply_stroke(gg, gr);
    CHECK(gg.sample({0, 0, r + 0.45}) < 0);
    CHECK(validate(dual_contour(gg), {.self_intersections = false}).ok);
}

TEST_CASE("chunk meshes tile the welded surface", "[sdf]") {
    Grid g = sphere_grid(3.0, 0.1);
    std::size_t tris = 0;
    for (const auto& c : chunks_of(g)) tris += dual_contour_chunk(g, c).F.size();
    CHECK(tris == dual_contour(g).F.size());
    // a stroke changes only the keys of chunks it touched
    std::map<std::tuple<int, int, int>, std::uint64_t> before;
    for (const auto& c : chunks_of(g)) before[{c.x, c.y, c.z}] = g.chunk_key(c);
    Stroke s{Brush::Draw, {{0, 0, 3.0}}, 0.5, 1.0};
    apply_stroke(g, s);
    int changed = 0, same = 0;
    for (const auto& c : chunks_of(g)) (g.chunk_key(c) == before[{c.x, c.y, c.z}] ? same : changed)++;
    CHECK(changed >= 1);
    CHECK(same > changed);
}
