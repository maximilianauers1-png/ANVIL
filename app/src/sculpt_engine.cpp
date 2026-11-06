#include "sculpt_engine.hpp"

#include <atomic>
#include <chrono>
#include <unordered_set>

#include "scene_builder.hpp"

namespace anvil {

namespace {
// GPU mesh keys of live chunks: their own range (bit 63), unique per content
std::atomic<std::uint64_t> g_live_key{0};
}  // namespace

SculptEngine::SculptEngine(UpdateFn on_update, DoneFn on_done)
    : on_update_(std::move(on_update)), on_done_(std::move(on_done)) {
    th_ = std::thread([this] { loop(); });
}

SculptEngine::~SculptEngine() {
    {
        std::lock_guard lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    th_.join();
}

void SculptEngine::begin(const std::string& object, std::shared_ptr<const sdf::Grid> start, sdf::Brush brush,
                         double radius, double strength, sdf::StrokeOptions opt) {
    std::lock_guard lk(mu_);
    Job j{Job::Begin, object, std::move(start), {}, {}};
    j.stroke.brush = brush, j.stroke.radius = radius, j.stroke.strength = strength, j.stroke.opt = opt;
    q_.push_back(std::move(j));
    active_ = true;
    cv_.notify_one();
}

void SculptEngine::add(const Vec3& p) {
    std::lock_guard lk(mu_);
    q_.push_back({Job::Point, {}, nullptr, {}, p});
    cv_.notify_one();
}

void SculptEngine::end() {
    std::lock_guard lk(mu_);
    q_.push_back({Job::End, {}, nullptr, {}, {}});
    active_ = false;
    cv_.notify_one();
}

void SculptEngine::loop() {
    std::string object;
    std::unique_ptr<sdf::Grid> grid;
    std::unique_ptr<sdf::StrokeApplier> applier;
    sdf::Stroke stroke;
    auto remesh = [&](const std::vector<sdf::IVec3>& bricks, double t_dabs_ms) {
        if (bricks.empty() || !grid) return;
        auto t0 = std::chrono::steady_clock::now();
        // a chunk mesh reads one brick beyond its chunk: border bricks touch neighbours
        std::unordered_set<sdf::IVec3, sdf::IVec3Hash> chunks;
        for (const auto& b : bricks)
            for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) chunks.insert(sdf::chunk_of_brick({b.x + dx, b.y + dy, b.z + dz}));
        std::vector<ChunkUpdate> ups;
        for (const auto& c : chunks) {
            auto cm = sdf::chunk_mesh(*grid, c);
            std::shared_ptr<const MeshCpu> gpu;
            if (!cm->mesh.F.empty())
                gpu = SceneBuilder::chunk_to_gpu(*cm, (std::uint64_t(1) << 63) | g_live_key.fetch_add(1));
            ups.push_back({c, gpu});
        }
        const double ms = t_dabs_ms + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        on_update_(object, std::move(ups), ms);
    };
    for (;;) {
        std::vector<Job> jobs;
        {
            std::unique_lock lk(mu_);
            cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
            if (stop_) return;
            jobs.assign(std::make_move_iterator(q_.begin()), std::make_move_iterator(q_.end()));
            q_.clear();
        }
        // all queued points are applied before one remesh (catches up when behind)
        auto t0 = std::chrono::steady_clock::now();
        bool dirty = false;
        for (auto& j : jobs) {
            switch (j.kind) {
                case Job::Begin:
                    object = j.object;
                    grid = std::make_unique<sdf::Grid>(*j.start);  // copy: shares bricks with the document
                    stroke = j.stroke;
                    stroke.path.clear();
                    applier = std::make_unique<sdf::StrokeApplier>(*grid, stroke.brush, stroke.radius, stroke.strength, stroke.opt);
                    break;
                case Job::Point:
                    if (!applier) break;
                    stroke.path.push_back(j.p);
                    applier->add(j.p);
                    dirty = true;
                    break;
                case Job::End: {
                    if (!applier) break;
                    if (dirty) {
                        remesh(applier->take_touched(),
                               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                        dirty = false;
                    }
                    applier->finish();
                    remesh(applier->take_touched(), 0);
                    applier.reset();
                    if (!stroke.path.empty())
                        on_done_(object, std::make_shared<const sdf::Grid>(std::move(*grid)), stroke);
                    grid.reset();
                    break;
                }
            }
        }
        if (dirty && applier)
            remesh(applier->take_touched(),
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
}

}  // namespace anvil
