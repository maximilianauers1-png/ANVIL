// Dimensioned quantities (ADR 0006).
//
// Internal units: mm, kg, s, K, A, rad. The compile-time `Quantity<D>` rejects
// adding a length to an angle; the run-time `RtQuantity` is used by the
// expression parser and the Python bridge.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace anvil {

/// Exponents of the base dimensions: Length, Mass, Time, Temperature, Current, Angle.
struct Dim {
    std::int8_t L = 0, M = 0, T = 0, Th = 0, I = 0, A = 0;
    constexpr bool operator==(const Dim&) const = default;
    constexpr Dim operator+(const Dim& o) const {
        return {std::int8_t(L + o.L), std::int8_t(M + o.M), std::int8_t(T + o.T),
                std::int8_t(Th + o.Th), std::int8_t(I + o.I), std::int8_t(A + o.A)};
    }
    constexpr Dim operator-(const Dim& o) const {
        return {std::int8_t(L - o.L), std::int8_t(M - o.M), std::int8_t(T - o.T),
                std::int8_t(Th - o.Th), std::int8_t(I - o.I), std::int8_t(A - o.A)};
    }
    constexpr Dim scaled(int k) const {
        return {std::int8_t(L * k), std::int8_t(M * k), std::int8_t(T * k),
                std::int8_t(Th * k), std::int8_t(I * k), std::int8_t(A * k)};
    }
    constexpr bool dimensionless() const { return *this == Dim{}; }
};

namespace dims {
inline constexpr Dim kNone{};
inline constexpr Dim kLength{1, 0, 0, 0, 0, 0};
inline constexpr Dim kAngle{0, 0, 0, 0, 0, 1};
inline constexpr Dim kArea{2, 0, 0, 0, 0, 0};
inline constexpr Dim kVolume{3, 0, 0, 0, 0, 0};
}  // namespace dims

/// Human readable name of a dimension, e.g. "Length", "Length^2", "Angle".
std::string dim_name(const Dim& d);

template <Dim D>
struct Quantity {
    double v = 0.0;  // value in internal units
    constexpr Quantity() = default;
    constexpr explicit Quantity(double x) : v(x) {}
    constexpr double value() const { return v; }
    constexpr Quantity operator+(Quantity o) const { return Quantity(v + o.v); }
    constexpr Quantity operator-(Quantity o) const { return Quantity(v - o.v); }
    constexpr Quantity operator-() const { return Quantity(-v); }
    constexpr Quantity operator*(double s) const { return Quantity(v * s); }
    constexpr Quantity operator/(double s) const { return Quantity(v / s); }
    constexpr auto operator<=>(const Quantity&) const = default;
    template <Dim E>
    constexpr Quantity<D + E> operator*(Quantity<E> o) const { return Quantity<D + E>(v * o.v); }
    template <Dim E>
    constexpr Quantity<D - E> operator/(Quantity<E> o) const { return Quantity<D - E>(v / o.v); }
};
template <Dim D>
constexpr Quantity<D> operator*(double s, Quantity<D> q) { return Quantity<D>(s * q.v); }

using Length = Quantity<dims::kLength>;
using Angle = Quantity<dims::kAngle>;
using Area = Quantity<dims::kArea>;
using Volume = Quantity<dims::kVolume>;

namespace units {
// Exact by definition: 1 inch = 25.4 mm (international yard and pound, 1959).
inline constexpr double kMmPerInch = 25.4;
inline constexpr Length mm{1.0};
inline constexpr Length cm{10.0};
inline constexpr Length m{1000.0};
inline constexpr Length inch{kMmPerInch};
inline constexpr Angle rad{1.0};
inline constexpr Angle deg{3.14159265358979323846 / 180.0};
}  // namespace units

/// Run-time quantity (value in internal units + dimension).
struct RtQuantity {
    double value = 0.0;
    Dim dim{};
};

/// Display units for lengths. Changing the display unit never changes stored values.
enum class LengthUnit : std::uint8_t { Millimeter, Centimeter, Meter, Inch };

struct LengthUnitInfo {
    LengthUnit unit;
    const char* symbol;  // "mm", "cm", "m", "in"
    double mm_per_unit;
};
const std::array<LengthUnitInfo, 4>& length_units();
const LengthUnitInfo& unit_info(LengthUnit u);
std::optional<LengthUnit> parse_length_unit(std::string_view symbol);

/// Formats a length (mm) in the display unit with `sig` significant digits and
/// trailing zeros removed, e.g. format_length(25.4, Inch) == "1 in".
std::string format_length(double mm, LengthUnit u, int sig = 6);

/// Shortest decimal representation that round-trips the double exactly.
std::string repr_double(double x);

}  // namespace anvil
