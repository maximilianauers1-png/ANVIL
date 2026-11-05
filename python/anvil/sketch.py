"""2D sketches with constraints (M2).

    s = Sketch("XY")
    a, b, c, d = s.rect((0, 0), (40, 20))          # 4 lines, horizontal/vertical, joined
    s.dim("length", a, 40*mm); s.dim("length", b, "h")
    s.fix(s.start(a))
    hole = s.circle((20, 10), r=2.6*mm)
    part = extrude(s, 5*mm)

Coordinates without a unit are mm (sketch plane coordinates); dimensions need
a unit or an expression string, like every other parameter.
"""
from __future__ import annotations

import math

import _anvil as _a

from .units import ANGLE, LENGTH, Quantity, mm

__all__ = ["Sketch"]

_DIM_KINDS = {"distance", "hdistance", "vdistance", "length", "radius", "diameter", "angle", "fix"}


def _coord(v) -> float:
    if isinstance(v, Quantity):
        if v.dim != LENGTH:
            raise TypeError("sketch coordinates must be lengths")
        return v.value
    return float(v)


def _val(v, kind: str) -> tuple:
    dim = ANGLE if kind == "angle" else LENGTH
    if isinstance(v, Quantity):
        if v.dim != dim:
            raise TypeError(f"{kind} needs a {'angle' if dim == ANGLE else 'length'}")
        return (v.value, "")
    if isinstance(v, str):
        return (0.0, v)
    raise TypeError(f"{kind} value needs a unit or an expression, e.g. 40*mm or \"2*w\"")


class Sketch:
    """Editable sketch. Pass to extrude(), revolve(), loft(), sweep(), solidify()."""

    def __init__(self, plane: str = "XY", offset=0 * mm, points=None, curves=None, constraints=None):
        if plane not in ("XY", "XZ", "YZ"):
            raise ValueError("Sketch plane must be 'XY', 'XZ' or 'YZ'")
        self.plane = plane
        self.offset = _coord(offset)
        self.points = {k: (float(v[0]), float(v[1])) for k, v in (points or {}).items()}
        self.curves = {}
        for k, v in (curves or {}).items():  # ("line", "P1", "P2"[, "construction"]) etc.
            kind, rest = v[0], list(v[1:])
            constr = bool(rest) and rest[-1] == "construction"
            if constr:
                rest = rest[:-1]
            if kind == "circle":
                self.curves[k] = ("circle", [rest[0]], float(rest[1]), constr)
            else:
                self.curves[k] = (kind, list(rest), 0.0, constr)
        self.constraints = {}
        for k, v in (constraints or {}).items():  # ("length", ["L1"], 40*mm)
            kind, refs = v[0], list(v[1])
            a = _val(v[2], kind) if len(v) > 2 else (0.0, "")
            b = _val(v[3], kind) if len(v) > 3 else (0.0, "")
            self.constraints[k] = (kind, refs, a, b)

    # ------------------------------------------------------------ internals
    def _id(self, prefix: str) -> str:
        i = 1
        while f"{prefix}{i}" in self.points or f"{prefix}{i}" in self.curves or f"{prefix}{i}" in self.constraints:
            i += 1
        return f"{prefix}{i}"

    def _pt(self, p) -> str:
        """Point id, or a new point for an (x, y) pair (reusing an existing point at the same spot)."""
        if isinstance(p, str):
            if p not in self.points:
                raise KeyError(f"no point '{p}' in this sketch")
            return p
        x, y = _coord(p[0]), _coord(p[1])
        for k, (px, py) in self.points.items():
            if abs(px - x) < 1e-9 and abs(py - y) < 1e-9:
                return k
        k = self._id("P")
        self.points[k] = (x, y)
        return k

    def _dict(self) -> dict:
        return {"plane": self.plane, "offset": self.offset, "points": self.points,
                "curves": self.curves, "constraints": self.constraints}

    def _node(self):
        return _a.make_sketch(self._dict())

    def _node_solved(self):
        node, _ = _a.sketch_solve(self._node())
        return node

    @classmethod
    def _from_node(cls, node) -> "Sketch":
        d = _a.sketch_dict(node)
        s = cls(d["plane"], d["offset"] * mm)
        s.points = {k: tuple(v) for k, v in d["points"].items()}
        s.curves = {k: (v[0], list(v[1]), v[2], v[3]) for k, v in d["curves"].items()}
        s.constraints = {k: (v[0], list(v[1]), tuple(v[2]), tuple(v[3])) for k, v in d["constraints"].items()}
        return s

    # ------------------------------------------------------------ geometry
    def point(self, x, y) -> str:
        return self._pt((x, y))

    def line(self, p, q, construction: bool = False) -> str:
        a, b = self._pt(p), self._pt(q)
        if a == b:
            raise ValueError("a line needs two different points")
        k = self._id("L")
        self.curves[k] = ("line", [a, b], 0.0, construction)
        return k

    def polyline(self, points, closed: bool = False, hv: bool = True) -> list:
        """Connected lines through the points (shared corners). hv=True adds
        horizontal/vertical constraints to segments that are axis-aligned."""
        ids = [self._pt(p) for p in points]
        if closed and ids[0] != ids[-1]:
            ids.append(ids[0])
        lines = []
        for a, b in zip(ids, ids[1:]):
            k = self.line(a, b)
            lines.append(k)
            if hv:
                (ax, ay), (bx, by) = self.points[a], self.points[b]
                if abs(ay - by) < 1e-9:
                    self.constrain("horizontal", k)
                elif abs(ax - bx) < 1e-9:
                    self.constrain("vertical", k)
        return lines

    def rect(self, corner, opposite) -> list:
        (x0, y0), (x1, y1) = corner, opposite
        return self.polyline([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], closed=True)

    def circle(self, center, r=None, d=None, construction: bool = False) -> str:
        if (r is None) == (d is None):
            raise TypeError("circle(): give r or d")
        rv = _coord(r) if r is not None else _coord(d) / 2
        c = self._pt(center)
        k = self._id("C")
        self.curves[k] = ("circle", [c], rv, construction)
        return k

    def arc(self, center, start, end, construction: bool = False) -> str:
        """Counter-clockwise arc from start to end around center."""
        c, s, e = self._pt(center), self._pt(start), self._pt(end)
        k = self._id("A")
        self.curves[k] = ("arc", [c, s, e], 0.0, construction)
        return k

    def start(self, curve: str) -> str:
        kind, pts, _, _ = self.curves[curve]
        return pts[0] if kind == "line" else pts[1]

    def end(self, curve: str) -> str:
        kind, pts, _, _ = self.curves[curve]
        return pts[1] if kind == "line" else pts[2]

    # ------------------------------------------------------------ constraints
    def constrain(self, kind: str, *refs) -> str:
        """Geometric constraint: coincident, horizontal, vertical, parallel,
        perpendicular, tangent, equal, point_on_line, point_on_circle,
        midpoint, symmetric, concentric."""
        if kind in _DIM_KINDS:
            raise ValueError(f"'{kind}' carries a value: use dim('{kind}', ..., value)")
        k = self._id("K")
        self.constraints[k] = (kind, list(refs), (0.0, ""), (0.0, ""))
        return k

    def dim(self, kind: str, *refs_and_value) -> str:
        """Dimension: distance, hdistance, vdistance, length, radius, diameter, angle."""
        if kind not in _DIM_KINDS or kind == "fix":
            raise ValueError(f"'{kind}' is not a dimension")
        *refs, value = refs_and_value
        k = self._id("K")
        self.constraints[k] = (kind, list(refs), _val(value, kind), (0.0, ""))
        return k

    def fix(self, point, x=None, y=None) -> str:
        p = self._pt(point) if not isinstance(point, str) else point
        px, py = self.points[p]
        k = self._id("K")
        self.constraints[k] = ("fix", [p], _val(x if x is not None else px * mm, "fix"),
                               _val(y if y is not None else py * mm, "fix"))
        return k

    def solve(self) -> dict:
        """Solves in place; returns dof, message and per-entity state (fixed/free)."""
        node, status = _a.sketch_solve(self._node())
        solved = Sketch._from_node(node)
        self.points, self.curves = solved.points, solved.curves
        return status

    def __repr__(self):
        return self._node().to_python()
