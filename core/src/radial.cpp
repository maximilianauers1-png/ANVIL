#include "anvil/radial.hpp"

#include <numbers>

namespace anvil {

int radial_sector(double dx, double dy, int n, double dead_zone_px) {
    if (n <= 0 || std::hypot(dx, dy) < dead_zone_px) return -1;
    const double w = 2.0 * std::numbers::pi / n;
    double theta = std::atan2(-dy, dx);  // screen y down -> math y up
    int k = static_cast<int>(std::floor((theta + w / 2) / w));
    return ((k % n) + n) % n;
}

void RadialGesture::begin(double x, double y, RadialRingProvider rings) {
    rings_ = std::move(rings);
    path_.clear();
    ring_ = rings_(path_);
    cx_ = x, cy_ = y;
    highlight_ = -1;
    active_ = true;
}

bool RadialGesture::move(double x, double y) {
    if (!active_) return false;
    const int n = static_cast<int>(ring_.size());
    int s = radial_sector(x - cx_, y - cy_, n, dead_zone_px);
    bool changed = s != highlight_;
    highlight_ = s;
    // Travelling far in the direction of a submenu opens it, centered here.
    if (s >= 0 && ring_[s].enabled && ring_[s].submenu && std::hypot(x - cx_, y - cy_) > submenu_radius_px) {
        path_.push_back(s);
        ring_ = rings_(path_);
        cx_ = x, cy_ = y;
        highlight_ = -1;
        changed = true;
    }
    return changed;
}

std::optional<std::vector<int>> RadialGesture::end(double x, double y) {
    if (!active_) return std::nullopt;
    move(x, y);  // a flick may end before any move event was processed
    active_ = false;
    if (highlight_ < 0 || !ring_[highlight_].enabled || ring_[highlight_].submenu) return std::nullopt;
    auto p = path_;
    p.push_back(highlight_);
    return p;
}

}  // namespace anvil
