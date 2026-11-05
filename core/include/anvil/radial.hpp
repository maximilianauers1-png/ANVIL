// Marking-menu gesture recognition (M0 plan §3.2), independent of drawing.
//
// Sector k = floor((theta + pi/N) / (2 pi / N)) mod N with theta = atan2(-dy, dx)
// (screen y points down), sector 0 = east, counter-clockwise. With angular
// selection the effective target width at distance D is W = 2 pi D / N, so the
// Fitts index of difficulty log2(1 + D/W) = log2(1 + N / 2 pi) is independent of D
// (1.18 bit for N = 8).
#pragma once
#include <cmath>
#include <functional>
#include <optional>
#include <vector>

namespace anvil {

/// -1 inside the dead zone, otherwise 0..n-1.
int radial_sector(double dx, double dy, int n, double dead_zone_px);
/// Fitts index of difficulty (bits) of an angular menu with n sectors.
inline double radial_fitts_id(int n) { return std::log2(1.0 + n / (2.0 * 3.14159265358979323846)); }

struct RadialItem {
    bool enabled = true;
    bool submenu = false;  // opens a nested ring when the pointer travels past `submenu_radius_px`
};

/// Ring description provider: given the path of submenu indices, returns the
/// items of that ring (size N <= 8; empty slots are disabled items).
using RadialRingProvider = std::function<std::vector<RadialItem>(const std::vector<int>& path)>;

class RadialGesture {
  public:
    double dead_zone_px = 12;
    double submenu_radius_px = 110;

    void begin(double x, double y, RadialRingProvider rings);
    /// Returns true if the highlighted sector or ring changed.
    bool move(double x, double y);
    /// Final selection: path of sector indices (last one is the command), or
    /// nullopt for cancel (released in the dead zone or on a disabled slot).
    std::optional<std::vector<int>> end(double x, double y);
    void cancel() { active_ = false; }

    bool active() const { return active_; }
    int highlighted() const { return highlight_; }
    const std::vector<int>& path() const { return path_; }
    double center_x() const { return cx_; }
    double center_y() const { return cy_; }

  private:
    RadialRingProvider rings_;
    std::vector<RadialItem> ring_;
    std::vector<int> path_;
    double cx_ = 0, cy_ = 0;
    int highlight_ = -1;
    bool active_ = false;
};

}  // namespace anvil
