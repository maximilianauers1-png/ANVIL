// Central tolerances (ADR 0006). Mirror: tests/reference/tolerances.py.
// No tolerance literal may appear anywhere else in the code base.
#pragma once
#include <algorithm>
#include <cmath>
#include <numbers>

namespace anvil::tol {

// --- Level 0: floating point arithmetic --------------------------------------
// 16 ulp of double: comparisons of quantities produced by a handful of roundings.
inline constexpr double kRelNumeric = 16.0 * 2.220446049250313e-16;
// Absolute floor near zero (mm). 1 pm.
inline constexpr double kAbsNumericMm = 1e-9;

// --- Level 1: geometry kernel --------------------------------------------------
// Point coincidence in model space (equals OCCT Precision::Confusion in mm).
inline constexpr double kConfusionMm = 1e-7;
// Degenerate triangle: area <= kDegenerateAreaRel * (bbox diagonal)^2.
inline constexpr double kDegenerateAreaRel = 1e-14;
// Self-intersection predicates: a signed distance below
// kSelfIntersectRel * (local edge length) is treated as zero ("touching").
inline constexpr double kSelfIntersectRel = 1e-9;
// Feature edges for snapping/rendering: dihedral angle above this (rad).
inline constexpr double kFeatureEdgeAngleRad = 20.0 * std::numbers::pi / 180.0;

// --- Level 2: manufacturing ----------------------------------------------------
// Default chord error for viewport tessellation and the STL export dialog (mm).
// Equal values let the export reuse the viewport's cached meshes; 10 um is
// below the XY pixel of current resin printers (ADR 0006, M1 update).
inline constexpr double kDefaultChordMm = 0.01;
inline constexpr double kDefaultExportChordMm = kDefaultChordMm;
// Lower bound for user-provided chord errors (mm): below this the mesh explodes
// without any printable benefit (resin XY pixels are ~18-50 um).
inline constexpr double kMinChordMm = 1e-4;
// Self-intersection tolerance floor for float32 (STL) data: this many float32
// ulps of the largest coordinate. Boolean results contain faces that touch
// along sub-ulp slivers; rounding to float moves them by up to 1/2 ulp, which
// a purely relative tolerance (kSelfIntersectRel * L) would read as crossing.
// 16 ulps at 50 mm = 0.1 um (<< chord 10 um, << printer pixel 18 um; slicers
// work on 1 nm integers).
inline constexpr double kExportSliverUlps = 16.0;

// Sculpt voxels (ADR 0008). 50 um default: DC sagitta kappa h^2 / 8 = 3 um at a
// 0.1 mm radius; structures below 2h vanish (detail linter, M5). 10 um is the
// floor: half the XY pixel of the Mars 5 Ultra (~18 um), finer gains nothing.
inline constexpr double kDefaultVoxelMm = 0.05;
inline constexpr double kMinVoxelMm = 0.01;
// Bound on the voxels of an object's bounding box at the chosen voxel size
// (only the narrow band is stored, but classification visits the box; 1e10
// = a 107 mm cube at 50 um).
inline constexpr double kMaxSculptGridVoxels = 1e10;
// Fast winding number (Barill et al. 2018, SIGGRAPH): a cluster of triangles
// is replaced by its dipole when the query point is farther than beta times
// the cluster radius. beta = 2 is the paper's default; the first-order error
// then stays far below the 0.5 inside/outside threshold (ADR 0018).
inline constexpr double kWindingBeta = 2.0;
// Imported models (M4): default target height of a 28-32 mm scale mini (mm).
inline constexpr double kDefaultMiniHeightMm = 32.0;

/// |a-b| <= max(abs, rel*max(|a|,|b|)) -- symmetric, unlike many isclose variants.
inline bool close(double a, double b, double rel = kRelNumeric, double abs = kAbsNumericMm) {
    return std::abs(a - b) <= std::max(abs, rel * std::max(std::abs(a), std::abs(b)));
}

}  // namespace anvil::tol
