// Turntable camera with Z up, double precision. The GPU receives
// camera-relative float data (ADR 0007): positions are offset by -eye in
// double before conversion to float.
#pragma once
#include <array>
#include <cmath>

#include "anvil/mesh.hpp"

namespace anvil {

using Mat4 = std::array<double, 16>;  // column-major (GLSL/Vulkan convention)

struct Ray {
    Vec3 origin, dir;  // dir normalized
};

struct Camera {
    Vec3 target{0, 0, 0};
    double yaw = -0.7853981633974483;    // rad, around +Z
    double pitch = 0.6154797086703874;   // rad, elevation (isometric: atan(1/sqrt 2))
    double distance = 150.0;             // mm
    double fov_y = 0.6981317007977318;   // rad (40 deg)
    bool ortho = false;
    double width = 1280, height = 720;   // viewport in device pixels

    Vec3 forward() const;  // unit, from eye to target
    Vec3 right() const;    // unit, screen right
    Vec3 up() const;       // unit, screen up
    Vec3 eye() const { return target - forward() * distance; }

    /// View rotation (no translation): world (camera-relative) -> view.
    Mat4 view_rotation() const;
    /// Vulkan projection: reversed Z (near=1, far=0), Y down, infinite far plane
    /// for perspective.
    Mat4 projection() const;
    double near_plane() const;
    /// Half depth of the orthographic slab (mm): scales with the view so float
    /// depth keeps ~micrometre resolution (float eps * 2 * range).
    double ortho_range() const;

    /// World point -> pixel (x right, y down) and view depth (distance along forward).
    bool project(const Vec3& world, double& px, double& py, double* depth = nullptr) const;
    /// Pixel -> world ray.
    Ray ray(double px, double py) const;
    /// Size of one pixel in world units at the given view depth.
    double world_per_pixel(double depth) const;

    /// Look along -dir (dir points from target to eye), e.g. (0,0,1) = top view.
    void look_from(const Vec3& dir);
    void orbit(double dyaw, double dpitch);
    void pan_pixels(double dx, double dy);
    /// Zoom by factor around the pixel (keeps the point under the cursor fixed on `plane_depth`).
    void zoom_at(double factor, double px, double py);
    void frame(const Box3& box);
};

}  // namespace anvil
