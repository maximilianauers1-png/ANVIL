// Client of the anvil-geom worker process (ADR 0001): OpenCASCADE runs in a
// separate process, so a kernel crash or hang can never take down the UI.
//
// Protocol over the child's stdin/stdout (binary): every message is
//   uint32 little-endian length | payload
// Request payload: JSON {"op": "eval"|"step_export"|"ping", ...}.
// Response: one JSON frame {"ok":bool,"error":str,"nv":N,"nt":M}, followed for
// meshes by one binary frame with N*3 float64 positions and M*3 uint32 indices.
#pragma once
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "anvil/mesh.hpp"

namespace anvil {

class Subprocess;

class GeomClient {
  public:
    explicit GeomClient(std::filesystem::path worker_exe,
                        std::chrono::milliseconds timeout = std::chrono::seconds(120));
    ~GeomClient();

    /// Evaluates a B-Rep tree (see brep_json) to a triangle mesh. Throws
    /// GeometryError with the kernel's message, or with a crash/timeout report.
    std::shared_ptr<const Mesh> eval(const std::string& tree_json, double chord_mm);
    /// Writes the tree as STEP AP214 (exact geometry).
    void export_step(const std::string& tree_json, const std::filesystem::path& path);
    /// Number of worker (re)starts so far.
    int starts() const { return starts_; }
    /// For tests: crash the worker on purpose.
    void debug_crash();

  private:
    std::string call(const std::string& request, std::string* payload);
    void ensure_running();

    std::filesystem::path exe_;
    std::chrono::milliseconds timeout_;
    std::unique_ptr<Subprocess> proc_;
    std::mutex mu_;
    int starts_ = 0;
};

/// Locates anvil-geom next to the running executable (or $ANVIL_GEOM).
std::filesystem::path default_geom_worker(const std::filesystem::path& exe_dir);

}  // namespace anvil
