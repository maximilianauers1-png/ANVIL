// Small numeric helpers shared by the drawing and move tools.
#pragma once
#include <cmath>

#include "anvil/tolerance.hpp"

namespace anvil::view_util {

inline double nice_step(double raw) {  // 1, 2, 5 x 10^k
    double p = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (m * p >= raw) return m * p;
    return 10 * p;
}

// Removes floating-point noise from picked coordinates: values are rounded to
// tol::kAbsNumericMm (below every geometric tolerance), and -0 becomes 0, so
// 3.0000000000000284 is stored as 3 and the generated command reads cleanly.
// Rounding is done as round(v*N)/N with an exact integer N: the final division
// of two exactly representable numbers is correctly rounded (IEEE 754), so
// decimal values come out as their nearest double (multiplying by 1e-9 would not).
inline double round_decimal(double v, int decimals) {
    if (decimals <= 0) {
        double s = std::pow(10.0, -decimals);
        return std::round(v / s) * s;
    }
    double n = std::pow(10.0, decimals);  // exact for decimals <= 22
    return std::round(v * n) / n;
}

inline double clean(double v) {
    const int dec = int(std::lround(-std::log10(tol::kAbsNumericMm)));
    double r = round_decimal(v, dec);
    return r == 0 ? 0.0 : r;
}

// Free (unsnapped) points are only known to about one pixel: keep the largest
// decimal step not coarser than a pixel, 10^floor(log10(wpp)).
inline double quantize_px(double v, double wpp) {
    int dec = -int(std::floor(std::log10(std::max(wpp, tol::kAbsNumericMm))));
    return round_decimal(v, dec);
}

}  // namespace anvil::view_util
