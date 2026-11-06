// G6 sculpt latency (CPU part): a stroke replayed at 60 Hz pointer samples on
// a grid with ~20 M active voxels (ADR 0008 reading: 20 M voxels in the band).
// Per frame: the new pointer sample is applied (all complete dabs) and the
// touched chunks are re-meshed -- exactly the work of SculptEngine before the
// upload. Prints JSON.
//
//   anvil-sculpt-bench [radius_mm=2] [voxel_mm=0.05] [sphere_r_mm=17]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <unordered_set>
#include <vector>

#include "anvil/sdf.hpp"

using namespace anvil;
using namespace anvil::sdf;
using Clock = std::chrono::steady_clock;

static double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

static double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, std::size_t(p * (v.size() - 1) + 0.5))];
}

int main(int argc, char** argv) {
    const double R = argc > 1 ? std::atof(argv[1]) : 2.0;
    const double h = argc > 2 ? std::atof(argv[2]) : 0.05;
    const double rs = argc > 3 ? std::atof(argv[3]) : 17.0;
    Box3 b;
    b.add(Vec3{-rs, -rs, -rs});
    b.add(Vec3{rs, rs, rs});
    auto t0 = Clock::now();
    Grid g = from_function([&](const Vec3& p) { return norm(p) - rs; }, b, h);
    const double t_build = ms_since(t0);
    t0 = Clock::now();
    auto parts = chunk_meshes(g);
    const double t_mesh = ms_since(t0);
    std::size_t tris = 0;
    for (const auto& p : parts) tris += p->mesh.F.size();
    t0 = Clock::now();
    Mesh w = weld(parts);
    const double t_weld = ms_since(t0);

    std::printf("{\n \"voxel_mm\": %g, \"radius_mm\": %g, \"active_voxels\": %zu, \"bricks\": %zu, \"triangles\": %zu,\n",
                h, R, g.voxel_count(), g.brick_count(), tris);
    std::printf(" \"build_ms\": %.1f, \"mesh_all_chunks_ms\": %.1f, \"weld_ms\": %.1f,\n \"strokes\": [\n", t_build, t_mesh,
                t_weld);
    const char* names[] = {"draw", "smooth", "grab"};
    const Brush brushes[] = {Brush::Draw, Brush::Smooth, Brush::Grab};
    for (int bi = 0; bi < 3; ++bi) {
        Grid work = g;
        StrokeApplier a(work, brushes[bi], R, 0.5);
        // 60 Hz samples, 30 mm/s along a meridian (0.5 mm per frame), 2 s
        std::vector<double> frame_ms, dab_ms, mesh_ms;
        std::size_t remeshed = 0;
        std::size_t dabs0 = 0, max_chunks = 0;
        for (int f = 0; f <= 120; ++f) {
            const double phi = 0.3 + 0.5 * f / rs;
            Vec3 p{rs * std::sin(phi), 0.0, rs * std::cos(phi)};
            if (brushes[bi] == Brush::Grab) p = p * (1.0 + 0.002 * f);  // pull outwards a little
            auto t = Clock::now();
            a.add(p);
            dab_ms.push_back(ms_since(t));
            auto tm = Clock::now();
            auto touched = a.take_touched();
            std::unordered_set<IVec3, IVec3Hash> chunks;
            for (const auto& bc : touched)
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) chunks.insert(chunk_of_brick({bc.x + dx, bc.y + dy, bc.z + dz}));
            std::vector<IVec3> todo(chunks.begin(), chunks.end());
            remesh_chunks(work, todo, &remeshed);
            mesh_ms.push_back(ms_since(tm));
            frame_ms.push_back(ms_since(t));
            max_chunks = std::max(max_chunks, chunks.size());
        }
        (void)dabs0;
        std::printf("  {\"brush\": \"%s\", \"frames\": %zu, \"dabs\": %zu, \"p50_ms\": %.2f, \"p99_ms\": %.2f, \"max_ms\": %.2f, "
                    "\"max_chunks_per_frame\": %zu, \"dab_p50_ms\": %.2f, \"remesh_p50_ms\": %.2f, \"remeshed_chunks\": %zu}%s\n",
                    names[bi], frame_ms.size(), a.dabs(), pct(frame_ms, 0.5), pct(frame_ms, 0.99), pct(frame_ms, 1.0),
                    max_chunks, pct(dab_ms, 0.5), pct(mesh_ms, 0.5), remeshed, bi < 2 ? "," : "");
    }
    std::printf(" ]\n}\n");
}
