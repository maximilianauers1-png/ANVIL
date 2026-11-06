// Live sculpt strokes: applies dabs on a worker thread as pointer samples
// arrive, remeshes only the chunks the dabs touched and hands GPU-ready chunk
// meshes to the viewport. The GUI and render threads never wait for it
// (requirement 2.2). On release it reports the final grid and the stroke;
// the controller turns that into exactly one sculpt(...) command whose
// evaluation finds the grid in the cache (same StrokeApplier steps,
// bit-identical result).
#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "anvil/sdf.hpp"
#include "vk/renderer.hpp"

namespace anvil {

class SculptEngine {
  public:
    struct ChunkUpdate {
        sdf::IVec3 chunk;
        std::shared_ptr<const MeshCpu> mesh;  // null = chunk is empty now
    };
    using UpdateFn = std::function<void(const std::string& object, std::vector<ChunkUpdate>, double ms)>;
    using DoneFn = std::function<void(const std::string& object, std::shared_ptr<const sdf::Grid> grid, sdf::Stroke stroke)>;

    SculptEngine(UpdateFn on_update, DoneFn on_done);
    ~SculptEngine();

    void begin(const std::string& object, std::shared_ptr<const sdf::Grid> start, sdf::Brush brush, double radius_mm,
               double strength);
    void add(const Vec3& p);
    void end();
    bool active() const { return active_; }

  private:
    void loop();
    UpdateFn on_update_;
    DoneFn on_done_;
    std::thread th_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false, active_ = false;
    struct Job {
        enum Kind { Begin, Point, End } kind;
        std::string object;
        std::shared_ptr<const sdf::Grid> start;
        sdf::Stroke stroke;  // Begin: brush, radius, strength
        Vec3 p;
    };
    std::deque<Job> q_;
};

}  // namespace anvil
