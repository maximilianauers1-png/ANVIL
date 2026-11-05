"""Dimensioned quantities for the ANVIL Python API.

Internal units: mm (length), rad (angle). ``25*mm + 1*inch`` works,
``25*mm + 3*deg`` raises a precise error. Mirrors ``core/include/anvil/units.hpp``.
"""
from __future__ import annotations

import math
from numbers import Real

__all__ = ["Quantity", "mm", "cm", "m", "um", "inch", "deg", "rad", "LENGTH", "ANGLE", "NONE"]

# Exponents: (Length, Mass, Time, Temperature, Current, Angle)
NONE = (0, 0, 0, 0, 0, 0)
LENGTH = (1, 0, 0, 0, 0, 0)
ANGLE = (0, 0, 0, 0, 0, 1)
_NAMES = ("Length", "Mass", "Time", "Temperature", "Current", "Angle")


def dim_name(d: tuple) -> str:
    if d == NONE:
        return "Dimensionless"
    parts = []
    for n, e in zip(_NAMES, d):
        if e:
            parts.append(n if e == 1 else f"{n}^{e}")
    return "*".join(parts)


class Quantity:
    """A value in internal units with a physical dimension."""

    __slots__ = ("value", "dim")

    def __init__(self, value: float, dim: tuple = NONE):
        self.value = float(value)
        self.dim = tuple(dim)

    # --- arithmetic -------------------------------------------------------
    def _coerce(self, o, op):
        if isinstance(o, Quantity):
            return o
        if isinstance(o, Real):
            return Quantity(o, NONE)
        raise TypeError(f"unsupported operand for {op}: Quantity and {type(o).__name__}")

    def __add__(self, o):
        o = self._coerce(o, "+")
        if o.dim != self.dim:
            raise TypeError(f"cannot add {dim_name(o.dim)} to {dim_name(self.dim)}")
        return Quantity(self.value + o.value, self.dim)

    __radd__ = __add__

    def __sub__(self, o):
        o = self._coerce(o, "-")
        if o.dim != self.dim:
            raise TypeError(f"cannot subtract {dim_name(o.dim)} from {dim_name(self.dim)}")
        return Quantity(self.value - o.value, self.dim)

    def __rsub__(self, o):
        return self._coerce(o, "-") - self

    def __mul__(self, o):
        if isinstance(o, Quantity):
            return Quantity(self.value * o.value, tuple(a + b for a, b in zip(self.dim, o.dim)))._simplify()
        if isinstance(o, Real):
            return Quantity(self.value * o, self.dim)
        return NotImplemented

    def __rmul__(self, o):
        if isinstance(o, Real):
            return Quantity(o * self.value, self.dim)
        return NotImplemented

    def __truediv__(self, o):
        if isinstance(o, Quantity):
            return Quantity(self.value / o.value, tuple(a - b for a, b in zip(self.dim, o.dim)))._simplify()
        if isinstance(o, Real):
            return Quantity(self.value / o, self.dim)
        return NotImplemented

    def __rtruediv__(self, o):
        if isinstance(o, Real):
            return Quantity(o / self.value, tuple(-a for a in self.dim))._simplify()
        return NotImplemented

    def __pow__(self, k):
        if not isinstance(k, int):
            raise TypeError("dimensioned quantities need an integer exponent")
        return Quantity(self.value**k, tuple(a * k for a in self.dim))._simplify()

    def __neg__(self):
        return Quantity(-self.value, self.dim)

    def __pos__(self):
        return self

    def __abs__(self):
        return Quantity(abs(self.value), self.dim)

    def _simplify(self):
        return self.value if self.dim == NONE else self

    # --- comparison -------------------------------------------------------
    def _cmp(self, o):
        o = self._coerce(o, "comparison")
        if o.dim != self.dim:
            raise TypeError(f"cannot compare {dim_name(self.dim)} with {dim_name(o.dim)}")
        return o.value

    def __eq__(self, o):
        if not isinstance(o, (Quantity, Real)):
            return NotImplemented
        try:
            return self.value == self._cmp(o)
        except TypeError:
            return False

    def __lt__(self, o):
        return self.value < self._cmp(o)

    def __le__(self, o):
        return self.value <= self._cmp(o)

    def __gt__(self, o):
        return self.value > self._cmp(o)

    def __ge__(self, o):
        return self.value >= self._cmp(o)

    def __hash__(self):
        return hash((self.value, self.dim))

    # --- conversion -------------------------------------------------------
    def to(self, unit: "Quantity") -> float:
        """Numeric value in `unit`, e.g. (25.4*mm).to(inch) == 1.0."""
        if unit.dim != self.dim:
            raise TypeError(f"cannot express {dim_name(self.dim)} in {dim_name(unit.dim)}")
        return self.value / unit.value

    def __float__(self):
        if self.dim != NONE:
            raise TypeError(f"{dim_name(self.dim)} is not a plain number; use .to(mm) or .value")
        return self.value

    def __repr__(self):
        if self.dim == LENGTH:
            return f"{self.value!r}*mm"
        if self.dim == ANGLE:
            return f"{math.degrees(self.value)!r}*deg"
        return f"Quantity({self.value!r}, {dim_name(self.dim)})"

    def __format__(self, spec):
        if self.dim == LENGTH:
            return format(self.value, spec) + " mm"
        if self.dim == ANGLE:
            return format(math.degrees(self.value), spec) + " deg"
        return format(self.value, spec)


mm = Quantity(1.0, LENGTH)
um = Quantity(1e-3, LENGTH)
cm = Quantity(10.0, LENGTH)
m = Quantity(1000.0, LENGTH)
inch = Quantity(25.4, LENGTH)  # exact by definition (1959)
rad = Quantity(1.0, ANGLE)
deg = Quantity(math.pi / 180.0, ANGLE)

UNITS_BY_SYMBOL = {"mm": mm, "cm": cm, "m": m, "in": inch, "inch": inch}
