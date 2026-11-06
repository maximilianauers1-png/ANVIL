#include "scene_builder.hpp"

#include <chrono>

#include "anvil/tolerance.hpp"

namespace anvil {

SceneBuilder::SceneBuilder(ShapeCache& cache, Ready on_ready, GhostReady on_ghost)
    : cache_(cache), on_ready_(std::move(on_ready)), on_ghost_(std::move(on_ghost)) {
    th_ = std::thread([this] { loop(); });
}

SceneBuilder::~SceneBuilder() {
    {
        std::lock_guard lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    th_.join();
}

void SceneBuilder::request(std::shared_ptr<const DocState> state, std::uint64_t revision, double chord_mm) {
    {
        std::lock_guard lk(mu_);
        pending_ = std::move(state);  // latest wins: intermediate states are skipped
        pending_rev_ = revision;
        chord_ = chord_mm;
    }
    cv_.notify_one();
}

void SceneBuilder::request_ghost(NodePtr node, std::shared_ptr<const DocState> state, double chord_mm) {
    {
        std::lock_guard lk(mu_);
        if (node) ghost_ = std::make_pair(std::move(node), std::move(state));
        else ghost_.reset();
        ghost_pending_ = true;
        chord_ = chord_mm;
    }
    cv_.notify_one();
}

std::shared_ptr<const MeshCpu> SceneBuilder::to_gpu(const std::shared_ptr<const Mesh>& m) {
    auto it = gpu_cache_.find(m.get());
    if (it != gpu_cache_.end()) return it->second.second;
    auto g = std::make_shared<MeshCpu>();
    g->key = next_key_++;
    Box3 b = m->bounds();
    g->origin = b.empty() ? Vec3{} : b.center();
    g->pos.resize(m->V.size() * 3);
    for (std::size_t i = 0; i < m->V.size(); ++i) {
        Vec3 p = m->V[i] - g->origin;  // relative in double, then float (camera-relative pipeline)
        g->pos[3 * i] = float(p.x), g->pos[3 * i + 1] = float(p.y), g->pos[3 * i + 2] = float(p.z);
    }
    g->tris.resize(m->F.size() * 3);
    for (std::size_t i = 0; i < m->F.size(); ++i)
        for (int k = 0; k < 3; ++k) g->tris[3 * i + k] = m->F[i][k];
    for (const auto& e : feature_edges(*m, tol::kFeatureEdgeAngleRad)) g->edges.push_back(e[0]), g->edges.push_back(e[1]);
    for (const auto& e : feature_edges(*m, -1.0)) g->wire.push_back(e[0]), g->wire.push_back(e[1]);  // all edges
    gpu_cache_[m.get()] = {m, g};
    return g;
}

std::shared_ptr<const MeshCpu> SceneBuilder::chunk_to_gpu(const sdf::ChunkMesh& c, std::uint64_t key) {
    auto g = std::make_shared<MeshCpu>();
    g->key = key;
    const Mesh& m = c.mesh;
    Box3 b = m.bounds();
    g->origin = b.empty() ? Vec3{} : b.center();
    g->pos.resize(m.V.size() * 3);
    for (std::size_t i = 0; i < m.V.size(); ++i) {
        Vec3 p = m.V[i] - g->origin;
        g->pos[3 * i] = float(p.x), g->pos[3 * i + 1] = float(p.y), g->pos[3 * i + 2] = float(p.z);
    }
    g->tris.resize(m.F.size() * 3);
    for (std::size_t i = 0; i < m.F.size(); ++i)
        for (int k = 0; k < 3; ++k) g->tris[3 * i + k] = m.F[i][k];
    for (const auto& e : feature_edges(m, -1.0)) g->wire.push_back(e[0]), g->wire.push_back(e[1]);  // mesh view
    return g;
}

std::shared_ptr<const MeshCpu> SceneBuilder::lines_to_gpu(const std::shared_ptr<const Mesh>& l) {
    auto it = gpu_cache_.find(l.get());
    if (it != gpu_cache_.end()) return it->second.second;
    auto g = std::make_shared<MeshCpu>();
    g->key = next_key_++;
    Box3 b = l->bounds();
    g->origin = b.empty() ? Vec3{} : b.center();
    for (std::size_t i = 0; i < l->V.size(); ++i) {
        Vec3 p = l->V[i] - g->origin;
        g->pos.insert(g->pos.end(), {float(p.x), float(p.y), float(p.z)});
        g->edges.push_back(std::uint32_t(i));
    }
    gpu_cache_[l.get()] = {l, g};
    return g;
}

void SceneBuilder::loop() {
    for (;;) {
        std::shared_ptr<const DocState> state;
        std::uint64_t rev = 0;
        double chord;
        std::optional<std::pair<NodePtr, std::shared_ptr<const DocState>>> ghost;
        bool do_ghost = false;
        {
            std::unique_lock lk(mu_);
            cv_.wait(lk, [&] { return stop_ || pending_ || ghost_pending_; });
            if (stop_) return;
            state = std::move(pending_);
            pending_.reset();
            rev = pending_rev_;
            chord = chord_;
            if (ghost_pending_) {
                ghost = ghost_;
                do_ghost = true;
                ghost_pending_ = false;
            }
        }
        EvalSettings es;
        es.chord_mm = chord;
        if (state) es.lookup = state->lookup();
        else if (ghost) es.lookup = ghost->second->lookup();
        if (do_ghost) {
            std::shared_ptr<const MeshCpu> g;
            if (ghost) {
                try {
                    auto sh = evaluate(ghost->first, ghost->second->resolver(), es, &cache_);
                    g = to_gpu(sh->mesh);
                } catch (const std::exception&) {
                }
            }
            on_ghost_(g);
        }
        if (!state) continue;
        auto t0 = std::chrono::steady_clock::now();
        auto scene = std::make_shared<Scene>();
        scene->revision = rev;
        auto vars = state->resolver();
        std::map<const Mesh*, bool> alive;
        for (const auto& o : state->objects) {
            SceneObject so;
            so.name = o->name;
            so.visible = o->visible;
            try {
                auto sh = evaluate(o->root, vars, es, &cache_);
                if (sh->lines) {  // sketch: line geometry only
                    alive[sh->lines.get()] = true;
                    so.gpu = lines_to_gpu(sh->lines);
                    so.bounds = sh->lines->bounds();
                    so.manifold = true;
                    if (o->visible) scene->bounds.add(so.bounds);
                    scene->objects.push_back(std::move(so));
                    continue;
                }
                so.root = o->root;
                if (sh->grid) {  // sculpt: chunk parts
                    so.grid = sh->grid;
                    for (const auto& ch : sh->chunks) {
                        const Mesh* key = &ch->mesh;
                        alive[key] = true;
                        auto gi = gpu_cache_.find(key);
                        std::shared_ptr<const MeshCpu> gpu;
                        if (gi != gpu_cache_.end()) {
                            gpu = gi->second.second;
                        } else {
                            gpu = chunk_to_gpu(*ch, next_key_++);
                            gpu_cache_[key] = {std::shared_ptr<const Mesh>(ch, &ch->mesh), gpu};
                        }
                        so.parts.push_back(gpu);
                        so.part_chunks.push_back(ch->chunk);
                        auto sc = snap_cache_.find(key);
                        if (sc == snap_cache_.end() || sc->second->name != o->name)
                            snap_cache_[key] = make_snap_object(o->name, std::shared_ptr<const Mesh>(ch, &ch->mesh), {});
                        so.part_snaps.push_back(snap_cache_[key]);
                    }
                    so.bounds = sh->mesh->bounds();
                    so.triangles = sh->mesh->F.size();
                    so.volume = sh->mesh->signed_volume();
                    so.manifold = !sh->mesh->F.empty();
                    so.error = sh->error;
                    if (o->visible) scene->bounds.add(so.bounds);
                    scene->objects.push_back(std::move(so));
                    continue;
                }
                so.gpu = to_gpu(sh->mesh);
                alive[sh->mesh.get()] = true;
                auto sc = snap_cache_.find(sh->mesh.get());
                if (sc == snap_cache_.end() || sc->second->name != o->name)
                    snap_cache_[sh->mesh.get()] = make_snap_object(o->name, sh->mesh, analytic_features(o->root, vars));
                so.snap = snap_cache_[sh->mesh.get()];
                so.bounds = sh->mesh->bounds();
                so.triangles = sh->mesh->F.size();
                so.volume = sh->mesh->signed_volume();
                so.manifold = bool(sh->solid) || (sh->grid && !sh->mesh->F.empty());
                if (!sh->solid) so.error = sh->error;
                if (o->visible) scene->bounds.add(so.bounds);
            } catch (const std::exception& e) {
                so.error = e.what();
            }
            scene->objects.push_back(std::move(so));
        }
        // drop CPU copies of meshes that are no longer in the document
        for (auto it = gpu_cache_.begin(); it != gpu_cache_.end();)
            it = alive.count(it->first) ? std::next(it) : gpu_cache_.erase(it);
        for (auto it = snap_cache_.begin(); it != snap_cache_.end();)
            it = alive.count(it->first) ? std::next(it) : snap_cache_.erase(it);
        scene->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        on_ready_(scene);
    }
}

}  // namespace anvil
