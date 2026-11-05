#include "anvil/units.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace anvil {

std::string dim_name(const Dim& d) {
    if (d.dimensionless()) return "Dimensionless";
    static const char* names[] = {"Length", "Mass", "Time", "Temperature", "Current", "Angle"};
    const int e[] = {d.L, d.M, d.T, d.Th, d.I, d.A};
    std::string out;
    for (int i = 0; i < 6; ++i) {
        if (e[i] == 0) continue;
        if (!out.empty()) out += "*";
        out += names[i];
        if (e[i] != 1) out += "^" + std::to_string(e[i]);
    }
    return out;
}

const std::array<LengthUnitInfo, 4>& length_units() {
    static const std::array<LengthUnitInfo, 4> k{{
        {LengthUnit::Millimeter, "mm", 1.0},
        {LengthUnit::Centimeter, "cm", 10.0},
        {LengthUnit::Meter, "m", 1000.0},
        {LengthUnit::Inch, "in", units::kMmPerInch},
    }};
    return k;
}

const LengthUnitInfo& unit_info(LengthUnit u) { return length_units()[static_cast<int>(u)]; }

std::optional<LengthUnit> parse_length_unit(std::string_view s) {
    for (const auto& u : length_units())
        if (s == u.symbol) return u.unit;
    if (s == "inch") return LengthUnit::Inch;
    return std::nullopt;
}

std::string repr_double(double x) {
    if (std::isnan(x)) return "nan";
    if (std::isinf(x)) return x > 0 ? "inf" : "-inf";
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof(buf), x);  // shortest round-trip form
    if (r.ec != std::errc()) throw std::runtime_error("repr_double failed");
    std::string s(buf, r.ptr);
    // Keep Python-compatible float literals ("25" -> "25.0" is not needed: we
    // always multiply by a unit, so integers are fine).
    return s;
}

std::string format_length(double mm, LengthUnit u, int sig) {
    const auto& info = unit_info(u);
    double v = mm / info.mm_per_unit;
    if (v == 0.0) v = 0.0;  // no "-0"
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*g", sig, v);
    std::string s(buf);
    if (s.find('e') == std::string::npos && s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s + " " + info.symbol;
}

}  // namespace anvil
