#include "anvil/camera.hpp"

#include <algorithm>
#include <numbers>

namespace anvil {

namespace {
constexpr double kOrthoDepthRange = 1e6;  // mm, +/- range of the orthographic depth slab
constexpr double kMinDistance = 1e-3;     // mm
constexpr double kMaxDistance = 1e7;      // mm
constexpr double kPitchLimit = std::numbers::pi / 2;
}  // namespace

Vec3 Camera::forward() const {
    return -Vec3{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
}
Vec3 Camera::right() const { return {-std::sin(yaw), std::cos(yaw), 0.0}; }
Vec3 Camera::up() const { return cross(right(), forward()); }

double Camera::near_plane() const { return std::max(distance * 1e-3, 1e-4); }

Mat4 Camera::view_rotation() const {
    Vec3 r = right(), u = up(), f = forward();
    // rows: r, u, -f (view looks down -Z)
    return {r.x, u.x, -f.x, 0, r.y, u.y, -f.y, 0, r.z, u.z, -f.z, 0, 0, 0, 0, 1};
}

Mat4 Camera::projection() const {
    const double aspect = width / std::max(height, 1.0);
    const double t = std::tan(fov_y / 2);
    if (!ortho) {
        const double n = near_plane();
        // clip.x = x/(t*a), clip.y = -y/t, clip.z = n, clip.w = -z  -> depth = n/(-z)
        return {1 / (t * aspect), 0, 0, 0, 0, -1 / t, 0, 0, 0, 0, 0, -1, 0, 0, n, 0};
    }
    const double hh = distance * t, hw = hh * aspect;
    const double nr = -kOrthoDepthRange, fr = kOrthoDepthRange;
    // depth = (fr + z)/(fr - nr)  (reversed: near -> 1)
    return {1 / hw, 0, 0, 0, 0, -1 / hh, 0, 0, 0, 0, 1 / (fr - nr), 0, 0, 0, fr / (fr - nr), 1};
}

bool Camera::project(const Vec3& w, double& px, double& py, double* depth) const {
    Vec3 d = w - eye();
    double x = dot(d, right()), y = dot(d, up()), z = dot(d, forward());
    if (depth) *depth = z;
    const double t = std::tan(fov_y / 2), aspect = width / std::max(height, 1.0);
    double nx, ny;
    if (!ortho) {
        if (z <= near_plane()) return false;
        nx = x / (z * t * aspect);
        ny = y / (z * t);
    } else {
        nx = x / (distance * t * aspect);
        ny = y / (distance * t);
    }
    px = (nx + 1) * 0.5 * width;
    py = (1 - ny) * 0.5 * height;
    return true;
}

Ray Camera::ray(double px, double py) const {
    const double t = std::tan(fov_y / 2), aspect = width / std::max(height, 1.0);
    double nx = 2 * px / width - 1, ny = 1 - 2 * py / height;
    if (!ortho) return {eye(), normalized(forward() + right() * (nx * t * aspect) + up() * (ny * t))};
    Vec3 o = eye() + right() * (nx * distance * t * aspect) + up() * (ny * distance * t);
    return {o - forward() * kOrthoDepthRange * 0.5, forward()};
}

double Camera::world_per_pixel(double depth) const {
    const double t = std::tan(fov_y / 2);
    double h = ortho ? 2 * distance * t : 2 * std::max(depth, near_plane()) * t;
    return h / std::max(height, 1.0);
}

void Camera::look_from(const Vec3& dir) {
    Vec3 d = normalized(dir);
    pitch = std::asin(std::clamp(d.z, -1.0, 1.0));
    if (std::abs(d.x) + std::abs(d.y) > 1e-12) yaw = std::atan2(d.y, d.x);
    // straight top/bottom view: keep yaw so that +Y points up on screen
    else yaw = -std::numbers::pi / 2;
}

void Camera::orbit(double dyaw, double dpitch) {
    yaw += dyaw;
    pitch = std::clamp(pitch + dpitch, -kPitchLimit, kPitchLimit);
}

void Camera::pan_pixels(double dx, double dy) {
    double s = world_per_pixel(distance);
    target = target - right() * (dx * s) + up() * (dy * s);
}

void Camera::zoom_at(double factor, double px, double py) {
    // Keep the point under the cursor on the focal plane fixed.
    Ray before = ray(px, py);
    Vec3 p_before = ortho ? before.origin + before.dir * (kOrthoDepthRange * 0.5) : before.origin + before.dir * (distance / dot(before.dir, forward()));
    distance = std::clamp(distance * factor, kMinDistance, kMaxDistance);
    Ray after = ray(px, py);
    Vec3 p_after = ortho ? after.origin + after.dir * (kOrthoDepthRange * 0.5) : after.origin + after.dir * (distance / dot(after.dir, forward()));
    target = target + (p_before - p_after);
}

void Camera::frame(const Box3& box) {
    if (box.empty()) return;
    target = box.center();
    double r = std::max(box.diagonal() / 2, 1.0);
    distance = std::clamp(r / std::sin(fov_y / 2) * 1.1, kMinDistance, kMaxDistance);
}

}  // namespace anvil
