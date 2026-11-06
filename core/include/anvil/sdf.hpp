// Sparse-brick signed distance fields, Dual Contouring and sculpt brushes
// (ADR 0008, M0 plan section 3.7).
//
// Storage: bricks of 8^3 voxels on the lattice x = h * (i, j, k), values f/h
// as IEEE binary16 in the narrow band |f| <= kBand * h. Bricks are shared
// (copy-on-write), so a Grid copy is a cheap snapshot: undo states and the
// live sculpt preview share every brick a stroke did not touch.
// Outside allocated bricks the field is +kBand (outside) or, for bricks
// recorded in `interior`, -kBand.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

#include "anvil/mesh.hpp"

namespace anvil::sdf {

inline constexpr int kBrick = 8;          // voxels per brick edge
inline constexpr int kBrickVoxels = kBrick * kBrick * kBrick;
inline constexpr float kBand = 3.0f;      // narrow band half width in voxels
inline constexpr int kChunkBricks = 4;    // meshing chunk: 4^3 bricks = 32^3 cells

// IEEE 754 binary16 <-> binary32, round to nearest even.
std::uint16_t float_to_half(float f);
float half_to_float(std::uint16_t h);

struct IVec3 {
    int x = 0, y = 0, z = 0;
    bool operator==(const IVec3&) const = default;
};
struct IVec3Hash {
    std::size_t operator()(const IVec3& c) const noexcept {
        // 3 large odd constants (Teschner et al. 2003 spatial hashing)
        return std::size_t(std::uint64_t(std::uint32_t(c.x)) * 73856093u ^
                           std::uint64_t(std::uint32_t(c.y)) * 19349663u ^
                           std::uint64_t(std::uint32_t(c.z)) * 83492791u);
    }
};
inline int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

struct Brick {
    std::array<std::uint16_t, kBrickVoxels> v{};  // f/h as binary16
    // Mesh stamp: changes whenever a value the surface depends on changes.
    // Contouring only uses edges with |a| + |b| < 2.5 (in h), so edits among
    // values >= 2.5 (band upkeep by reinitialisation) keep it: chunk meshes
    // are rebuilt only where the surface can actually differ.
    std::uint64_t stamp = 0;
    static int index(int x, int y, int z) { return (z * kBrick + y) * kBrick + x; }
};

class Grid {
  public:
    explicit Grid(double h = 0.05) : h_(h) {}
    double h() const { return h_; }

    /// f/h at a lattice point (clamped to +-kBand outside the band).
    float at(int i, int j, int k) const;
    /// Signed distance in mm, trilinear.
    double sample(const Vec3& p) const;
    /// Gradient of the trilinear interpolant (finite differences of `sample`, h/2).
    Vec3 gradient(const Vec3& p) const;

    // --- editing (copy-on-write)
    Brick& mutable_brick(const IVec3& b);       // allocates (filled with the implied value) if needed
    void set(int i, int j, int k, float f_over_h);
    bool allocated(const IVec3& b) const { return bricks_.count(b) != 0; }
    bool is_interior(const IVec3& b) const { return interior_.count(b) != 0; }
    void mark_interior(const IVec3& b) { interior_.insert(b); }
    /// Frees bricks that are entirely +-kBand (outside / inside).
    void compact(const std::vector<IVec3>& candidates);

    std::size_t brick_count() const { return bricks_.size(); }
    std::size_t voxel_count() const { return bricks_.size() * kBrickVoxels; }
    std::size_t interior_count() const { return interior_.size(); }
    const std::unordered_map<IVec3, std::shared_ptr<Brick>, IVec3Hash>& bricks() const { return bricks_; }
    Box3 bounds() const;  // of the allocated bricks (mm)
    /// New mesh stamp for a brick after a surface-relevant edit.
    void touch_mesh(const IVec3& b);
    /// Mesh stamp of a brick (0/1 = not allocated, outside/inside).
    std::uint64_t brick_stamp(const IVec3& b) const;
    /// Key of a chunk's mesh: its bricks and the one-brick ring it reads.
    std::uint64_t chunk_key(const IVec3& chunk) const;

  private:
    double h_;
    std::unordered_map<IVec3, std::shared_ptr<Brick>, IVec3Hash> bricks_;
    std::unordered_set<IVec3, IVec3Hash> interior_;
};

/// Grid from an exact signed distance function (negative inside). `bounds`
/// must contain the zero set; bricks farther than the band from the surface
/// are classified by the sign at their center.
Grid from_function(const std::function<double(const Vec3&)>& f, const Box3& bounds, double h);

/// Grid from a closed, consistently oriented triangle mesh: unsigned distance
/// by BVH closest-point queries, sign from the angle-weighted pseudonormal of
/// the closest feature (Baerentzen & Aanaes 2005, IEEE TVCG 11(3)).
Grid from_mesh(const Mesh& m, double h);

/// Dual contouring (Ju et al. 2002 topology: one quad per sign-changing
/// lattice edge) of the cells owned by one chunk (chunk coordinates in units
/// of kChunkBricks bricks). Cell vertices sit at the mass point of the cell's
/// edge crossings (Surface Nets placement): measured fold-free where the QEF
/// minimiser self-intersected (ADR 0017).
Mesh dual_contour_chunk(const Grid& g, const IVec3& chunk);
/// Whole surface, welded (one vertex per cell): closed and exportable.
Mesh dual_contour(const Grid& g);

/// Chunk mesh plus the lattice cell of every vertex (for welding).
struct ChunkMesh {
    std::uint64_t key = 0;   // Grid::chunk_key at creation
    IVec3 chunk;
    Mesh mesh;
    std::vector<std::uint64_t> cell;  // packed cell coordinate per vertex
};
/// Cached by chunk key (process-wide LRU): unchanged chunks are not re-meshed.
std::shared_ptr<const ChunkMesh> chunk_mesh(const Grid& g, const IVec3& chunk);
/// Meshes of the given chunks, the uncached ones in parallel (live strokes).
/// `built` (optional) accumulates how many were actually re-meshed.
std::vector<std::shared_ptr<const ChunkMesh>> remesh_chunks(const Grid& g, const std::vector<IVec3>& chunks,
                                                            std::size_t* built = nullptr);
/// All chunk meshes of the grid (parallel for the uncached ones).
std::vector<std::shared_ptr<const ChunkMesh>> chunk_meshes(const Grid& g);
/// Welds chunk meshes into one closed mesh: vertices of the same cell merge
/// (they are bit-identical by construction). Radix sort of the packed cell
/// keys, O(V).
Mesh weld(const std::vector<std::shared_ptr<const ChunkMesh>>& parts);
/// Chunks containing at least one allocated brick.
std::vector<IVec3> chunks_of(const Grid& g);
inline IVec3 chunk_of_brick(const IVec3& b) {
    return {floor_div(b.x, kChunkBricks), floor_div(b.y, kChunkBricks), floor_div(b.z, kChunkBricks)};
}

/// First zero crossing along origin + t dir (dir unit), t in [0, t_max]:
/// sphere tracing (Hart 1996) with steps max(|f|, h/2) -- f is a distance
/// (1-Lipschitz) inside the band and >= kBand h outside -- then bisection of
/// the bracketing step. nullopt if the ray misses.
std::optional<double> raycast(const Grid& g, const Vec3& origin, const Vec3& dir, double t_max);

// --- sculpting ------------------------------------------------------------
enum class Brush { Draw, Carve, Smooth, Grab, Flatten };
const char* brush_name(Brush b);
bool parse_brush(const std::string& s, Brush& out);

/// One stroke: points along the path (mm), radius R (mm), strength s in
/// [0, 1]. Draw/Carve move the surface by up to s * R / 4 per stroke-length R
/// (Wendland C2 falloff w(q) = (1-q)^4 (1+4q), q = |x-c|/R); Smooth blends
/// towards the 6-neighbour mean; Grab carries the surface along the path;
/// Flatten pulls towards the plane through the first dab.
struct Stroke {
    Brush brush = Brush::Draw;
    std::vector<Vec3> path;
    double radius = 2.0;
    double strength = 0.5;
};

/// Incremental stroke application: points arrive one by one (live sculpting)
/// and every complete dab is applied immediately. Feeding the same points
/// gives bit-identical grids to apply_stroke (replay of the Python command).
class StrokeApplier {
  public:
    StrokeApplier(Grid& g, Brush brush, double radius, double strength);
    void add(const Vec3& p);
    void finish();
    /// Bricks changed since the last call.
    std::vector<IVec3> take_touched();
    std::size_t dabs() const { return dabs_; }

  private:
    void dab(const Vec3& c, const Vec3& grab);
    Grid& g_;
    Brush brush_;
    double R_, st_, spacing_;
    bool have_first_ = false, finished_ = false;
    Vec3 last_point_, last_dab_;
    double carry_ = 0;
    Vec3 plane_n_;
    double plane_d_ = 0;
    std::size_t dabs_ = 0;
    std::unordered_set<IVec3, IVec3Hash> touched_, all_touched_;
};

/// Applies the stroke; returns the bricks it changed (for remeshing).
std::vector<IVec3> apply_stroke(Grid& g, const Stroke& s);

}  // namespace anvil::sdf
