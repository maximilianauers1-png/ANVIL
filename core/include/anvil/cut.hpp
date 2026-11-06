// Cut tools (knife, straight cut, lasso, drill, slice): a cutter prism is the
// region on one side of a drawn path, extruded along the view direction
// through the whole object; the kept part is the object intersected with (or
// minus) the cutter. The boolean closes the cut face, so the result stays a
// closed solid ("the cut surface is filled").
//
// Region (2D, in the plane through path[0] perpendicular to `dir`, axes u, v):
//   knife: the open path, both end segments extended far beyond the object,
//          closed on its left side (left = (-d_y, d_x) of the segment direction);
//   lasso: the closed path itself (drill = a circle);
//   plane: the half-space dir . (x - path[0]) > 0 (no 2D region).
// Perspective (optional `eye`): the region is swept along the rays from the
// eye instead of parallel to `dir` (a frustum), so the cut follows exactly
// what was drawn on screen at every depth. 2D coordinates are then relative
// to the foot of the eye on the path plane: a point at depth d along `dir`
// maps to eye + n d + (u x + v y) d / d0, d0 = depth of the path plane.
// keep: knife left|right, lasso inside|outside, plane above|below; the first
// of each pair intersects with the cutter, the second subtracts it.
#pragma once
#include <array>
#include <map>
#include <string>
#include <vector>

#include "anvil/mesh.hpp"

namespace anvil::cut {

struct Spec {
    std::string mode = "knife";
    std::vector<Vec3> path;
    Vec3 dir{0, 0, -1};
    std::string keep = "left";
    bool has_eye = false;  // perspective sweep from `eye`
    Vec3 eye;
};

/// Parses the node attributes (path "x,y,z;x,y,z;...", dir "x,y,z", mode, keep);
/// throws std::runtime_error with a precise message.
Spec parse(const std::map<std::string, std::string>& attrs);
/// True when `keep` selects the cutter region itself (intersection).
bool keeps_region(const Spec& s);

struct Frame {
    Vec3 o, u, v, n;
    double d0 = 0;  // perspective: depth of the path plane from the eye
};
Frame frame(const Spec& s);

using Polygon2 = std::vector<std::array<double, 2>>;
/// Region polygons in frame coordinates, cleaned (union, non-zero fill).
/// `reach` (mm): how far the region must extend (beyond the object).
std::vector<Polygon2> region(const Spec& s, double reach);
/// Whether a world point lies in the cutter region (the "left"/"inside"/"above" part).
bool in_region(const Spec& s, const Vec3& p, double reach);

}  // namespace anvil::cut
