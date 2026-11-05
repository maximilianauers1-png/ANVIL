// Background evaluation: document state -> GPU-ready meshes + snap data.
// Runs on its own thread so neither the GUI nor the render thread waits for
// geometry kernels (requirement 2.2).
#pragma once
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "anvil/document.hpp"
#include "anvil/snap.hpp"
#include "vk/renderer.hpp"

namespace anvil {

struct SceneObject {
    std::string name;
    bool visible = true;
    std::shared_ptr<const MeshCpu> gpu;
    std::shared_ptr<const SnapObject> snap;
    Box3 bounds;
    std::size_t triangles = 0;
    double volume = 0;
    bool manifold = true;
    std::string error;
};

struct Scene {
    std::uint64_t revision = 0;
    std::vector<SceneObject> objects;
    Box3 bounds;
    double seconds = 0;  // evaluation time
};

class SceneBuilder {
  public:
    using Ready = std::function<void(std::shared_ptr<const Scene>)>;
    using GhostReady = std::function<void(std::shared_ptr<const MeshCpu>)>;

    SceneBuilder(ShapeCache& cache, Ready on_ready, GhostReady on_ghost);
    ~SceneBuilder();

    void request(std::shared_ptr<const DocState> state, std::uint64_t revision, double chord_mm);
    void request_ghost(NodePtr node, std::shared_ptr<const DocState> state, double chord_mm);

  private:
    void loop();
    std::shared_ptr<const MeshCpu> to_gpu(const std::shared_ptr<const Mesh>& m);
    std::shared_ptr<const MeshCpu> lines_to_gpu(const std::shared_ptr<const Mesh>& lines);

    ShapeCache& cache_;
    Ready on_ready_;
    GhostReady on_ghost_;
    std::thread th_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::shared_ptr<const DocState> pending_;
    std::uint64_t pending_rev_ = 0;
    double chord_ = 0.01;
    std::optional<std::pair<NodePtr, std::shared_ptr<const DocState>>> ghost_;
    bool ghost_pending_ = false;
    std::uint64_t next_key_ = 1;
    // Strong refs keep Mesh pointers unique while their GPU copies are alive.
    std::map<const Mesh*, std::pair<std::shared_ptr<const Mesh>, std::shared_ptr<const MeshCpu>>> gpu_cache_;
    std::map<const Mesh*, std::shared_ptr<const SnapObject>> snap_cache_;
};

}  // namespace anvil
