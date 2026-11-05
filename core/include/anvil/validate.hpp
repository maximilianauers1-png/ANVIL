// Printability checks run before every export (requirement 2.4).
// Specification and oracle: tests/reference/meshcheck.py.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "anvil/mesh.hpp"

namespace anvil {

enum class IssueKind {
    InvalidIndex,
    Degenerate,          // triangle area below tolerance
    OpenEdge,            // edge with one incident triangle
    NonManifoldEdge,     // edge with > 2 incident triangles
    InconsistentOrient,  // half-edge used twice in the same direction
    NonManifoldVertex,   // more than one triangle fan at a vertex
    EulerMismatch,       // V - E + F != 2(1 - g) with integer g >= 0
    NegativeVolume,      // shell with signed volume <= 0 (inside out)
    SelfIntersection,    // two non-adjacent triangles intersect
    Empty,
};

const char* issue_name(IssueKind k);

struct Issue {
    IssueKind kind;
    std::string message;          // precise, human-readable, English (UI language)
    std::int64_t index = -1;      // triangle / vertex index, if applicable
    std::int64_t index2 = -1;     // second triangle for self-intersections
    Vec3 location{};              // world position to highlight in the viewport
};

struct ShellInfo {
    std::size_t faces = 0, edges = 0, vertices = 0;
    long euler = 0;
    long genus = 0;
    double volume = 0.0;
};

struct ValidationReport {
    bool ok = true;
    std::vector<Issue> issues;  // capped per kind, counts are exact
    std::size_t counts[10] = {};
    std::vector<ShellInfo> shells;
    double volume = 0.0;
    double area = 0.0;
    std::size_t triangles = 0;
    double seconds = 0.0;

    std::string summary() const;
};

struct ValidationOptions {
    bool self_intersections = true;
    std::size_t max_issues_per_kind = 8;
    /// Absolute floor (mm) of the intersection tolerance. For float32 data
    /// (STL) it must not be below the representation error, else touching
    /// faces read as crossing; see float_tolerance().
    double abs_tol_mm = 0.0;
};

/// Intersection tolerance for float32 positions: tol::kExportSliverUlps ulps
/// of the largest coordinate magnitude.
double float_tolerance(const Mesh& m);

ValidationReport validate(const Mesh& m, const ValidationOptions& opt = {});

}  // namespace anvil
