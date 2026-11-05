// anvil-bench: CPU-side benchmarks of the geometry core on the BS-5M scene.
//
//   anvil-bench <bs5m.stl> [--out report.json] [--repeat 5]
//
// Measures (median of N runs, wall clock):
//   STL decode + vertex welding, validation (all export checks incl. the
//   self-intersection test), feature-edge extraction, BVH build, raycasts,
//   snap queries, magnet-base evaluation (Manifold) and binary STL encoding.
// GPU frame times come from `anvil --bench` (application), not from here.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "anvil/bvh.hpp"
#include "anvil/fsutil.hpp"
#include "anvil/geometry.hpp"
#include "anvil/snap.hpp"
#include "anvil/stl.hpp"
#include "anvil/tolerance.hpp"
#include "anvil/validate.hpp"
#include "anvil/version.hpp"

using namespace anvil;
using Clock = std::chrono::steady_clock;

namespace {
double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() / 2];
}
double time_ms(const std::function<void()>& f) {
    auto t = Clock::now();
    f();
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "--help") {
        std::printf("usage: anvil-bench <bs5m.stl> [--out report.json] [--repeat N]\n");
        return argc < 2 ? 2 : 0;
    }
    std::string path = argv[1], out;
    int repeat = 3;
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--out") out = argv[++i];
        else if (std::string(argv[i]) == "--repeat") repeat = std::max(1, std::atoi(argv[++i]));
    }
    std::vector<std::pair<std::string, double>> rows;
    auto bench = [&](const char* name, const std::function<void()>& f) {
        std::vector<double> t;
        for (int i = 0; i < repeat; ++i) t.push_back(time_ms(f));
        rows.emplace_back(name, median(t));
        std::printf("%-42s %10.1f ms\n", name, rows.back().second);
        std::fflush(stdout);
    };

    std::string bytes = read_file(path);
    StlReadResult rd;
    bench("stl decode + weld", [&] { rd = decode_stl(bytes); });
    const Mesh& m = rd.mesh;
    std::printf("  %zu triangles, %zu vertices\n", m.F.size(), m.V.size());

    ValidationReport rep;
    bench("validate (all checks)", [&] { rep = validate(m); });
    std::printf("  %s\n", rep.summary().c_str());
    ValidationOptions no_si;
    no_si.self_intersections = false;
    bench("validate (topology only)", [&] { validate(m, no_si); });
    bench("feature edges", [&] { feature_edges(m, tol::kFeatureEdgeAngleRad); });
    Bvh bvh;
    bench("bvh build", [&] { bvh = Bvh(m); });

    std::mt19937 rng(1);
    Box3 b = m.bounds();
    std::uniform_real_distribution<double> ux(b.lo.x, b.hi.x), uy(b.lo.y, b.hi.y);
    const int rays = 100000;
    int hits = 0;
    double ray_ms = 0;
    {
        std::vector<double> t;
        for (int r = 0; r < repeat; ++r)
            t.push_back(time_ms([&] {
                hits = 0;
                for (int i = 0; i < rays; ++i)
                    if (bvh.raycast({ux(rng), uy(rng), b.hi.z + 10}, {0, 0, -1})) ++hits;
            }));
        ray_ms = median(t);
    }
    rows.emplace_back("raycast (per ray, us)", ray_ms * 1000.0 / rays);
    std::printf("%-42s %10.3f us  (%d/%d hit)\n", "raycast per ray", ray_ms * 1000.0 / rays, hits, rays);

    // snap queries on the full scene (one object = worst case for the neighbourhood search)
    auto mesh = std::make_shared<Mesh>(m);
    SnapScene scene;
    scene.objects.push_back(make_snap_object("scene", mesh, {}));
    SnapEngine eng;
    SnapQuery q;
    q.cam.width = 2560, q.cam.height = 1440;
    q.cam.frame(b);
    std::vector<double> snap_t;
    std::uniform_real_distribution<double> px(0, 2560), py(0, 1440);
    for (int i = 0; i < 2000; ++i) {
        q.mx = px(rng), q.my = py(rng);
        snap_t.push_back(time_ms([&] { eng.update(scene, q); }));
    }
    std::sort(snap_t.begin(), snap_t.end());
    double snap_p50 = snap_t[snap_t.size() / 2], snap_p99 = snap_t[snap_t.size() * 99 / 100];
    rows.emplace_back("snap query p50 (ms)", snap_p50);
    rows.emplace_back("snap query p99 (ms)", snap_p99);
    std::printf("%-42s %10.3f / %.3f ms (p50/p99, 2560x1440)\n", "snap query", snap_p50, snap_p99);

    // magnet base end-to-end (cold evaluation, no cache)
    auto base = make_boolean(BoolOp::Difference,
                             make_primitive(NodeKind::Cylinder, {{"d", {25, {}}}, {"h", {3, {}}}, {"chamfer_top", {0.6, {}}}}),
                             make_primitive(NodeKind::Cylinder, {{"d", {5.1, {}}}, {"h", {2.2, {}}}}));
    EvalSettings es;
    es.chord_mm = 0.01;
    bench("magnet base eval (Manifold, cold)", [&] { evaluate(base, {}, es); });
    bench("stl encode (BS-5M, binary)", [&] { encode_stl(m, {}); });

    if (!out.empty()) {
        std::ofstream o(out);
        o << "{\n  \"anvil\": \"" << version_string() << "\",\n  \"threads\": " << std::thread::hardware_concurrency()
          << ",\n  \"triangles\": " << m.F.size() << ",\n  \"results_ms\": {\n";
        for (std::size_t i = 0; i < rows.size(); ++i)
            o << "    \"" << rows[i].first << "\": " << rows[i].second << (i + 1 < rows.size() ? ",\n" : "\n");
        o << "  }\n}\n";
    }
    return rep.ok ? 0 : 1;
}
