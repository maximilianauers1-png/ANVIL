"""Public modelling API. The UI, scripts and (from M6) the AI use exactly these
functions; every UI action is written as a call to them (ADR 0005).

    from anvil import *
    base = cylinder(d=25*mm, h=3*mm).chamfer(top=0.6*mm)
    pocket = cylinder(d=5.1*mm, h=2.2*mm)
    part = base - pocket
    show(part)
    export_stl(part, "base_25mm.stl", unit=mm, chord=0.01*mm)
"""
from __future__ import annotations

import json as _json
import os as _os
from numbers import Real
from typing import Iterable, Union

import _anvil as _a

from .sketch import Sketch
from .units import ANGLE, LENGTH, NONE, Quantity, deg, dim_name, mm

__all__ = [
    "Solid", "Obj", "box", "cube", "cylinder", "cone", "sphere", "torus", "transform",
    "show", "hide", "unhide", "delete", "rename", "get", "objects", "var", "edit", "move",
    "subtract", "union", "intersect", "duplicate", "clear", "export_stl", "import_stl", "import_mesh",
    "check", "info", "save", "load", "Sketch", "extrude", "revolve", "fillet", "chamfer_edges", "shell",
    "pattern_linear", "pattern_circular", "loft", "sweep", "ref", "import_step", "import_step_solid",
    "export_step", "view", "theme", "display", "select", "set_unit", "GeometryError",
]

GeometryError = _a.GeometryError
_state = {"shown": False}  # whether show() ran in the current command (REPL convenience)

# --------------------------------------------------------------------------- params

_PARAM_DIMS = {
    "extrude": {"distance": LENGTH}, "revolve": {"angle": ANGLE}, "fillet": {"r": LENGTH},
    "chamfer": {"d": LENGTH}, "shell": {"t": LENGTH},
    "linear_pattern": {"dx": LENGTH, "dy": LENGTH, "dz": LENGTH}, "circular_pattern": {"angle": ANGLE},
    "box": {"x": LENGTH, "y": LENGTH, "z": LENGTH},
    "cylinder": {"d": LENGTH, "h": LENGTH, "d2": LENGTH, "chamfer_top": LENGTH, "chamfer_bottom": LENGTH},
    "sphere": {"d": LENGTH},
    "torus": {"R": LENGTH, "r": LENGTH},
    "transform": {"tx": LENGTH, "ty": LENGTH, "tz": LENGTH, "rx": ANGLE, "ry": ANGLE, "rz": ANGLE},
}


def _param(name: str, value, dim) -> tuple:
    """Quantity -> (value, ""), str -> (0, expression). Plain numbers are rejected
    for dimensioned parameters: '25' is ambiguous, '25*mm' is not."""
    if isinstance(value, Quantity):
        if value.dim != dim:
            raise TypeError(f"{name} must be a {dim_name(dim)}, got {dim_name(value.dim)}")
        return (value.value, "")
    if isinstance(value, str):
        return (0.0, value.strip())
    if isinstance(value, Real) and not isinstance(value, bool):
        if dim == NONE:
            return (float(value), "")
        unit = "mm" if dim == LENGTH else "deg"
        raise TypeError(f"{name}={value!r} has no unit; write {name}={value!r}*{unit}")
    raise TypeError(f"{name}: expected a quantity or an expression string, got {type(value).__name__}")


def _params(kind: str, kw: dict) -> dict:
    dims = _PARAM_DIMS[kind]
    out = {}
    for k, v in kw.items():
        if v is None:
            continue
        if k not in dims:
            raise TypeError(f"{kind}() got an unexpected parameter '{k}' (valid: {', '.join(dims)})")
        out[k] = _param(k, v, dims[k])
    return out


def _node(x):
    if isinstance(x, Sketch):
        return x._node()
    if isinstance(x, Solid):
        return x._node
    if isinstance(x, Obj):
        return _a.get(x.name)
    if isinstance(x, str):
        return _a.get(x)
    raise TypeError(f"expected a Solid, an object or an object name, got {type(x).__name__}")


def _name(x) -> str:
    if isinstance(x, Obj):
        return x.name
    if isinstance(x, str):
        return x
    raise TypeError(f"expected an object or object name, got {type(x).__name__}")


# --------------------------------------------------------------------------- Solid

class Solid:
    """An immutable geometry expression (a node of the feature DAG).

    Arithmetic builds booleans: a - b (difference), a | b or a + b (union),
    a & b (intersection). Nothing is evaluated until shown or exported."""

    __slots__ = ("_node",)

    def __init__(self, node):
        self._node = node

    def __sub__(self, o):
        return Solid(_a.make_boolean("difference", self._node, _node(o)))

    def __or__(self, o):
        return Solid(_a.make_boolean("union", self._node, _node(o)))

    __add__ = __or__

    def __and__(self, o):
        return Solid(_a.make_boolean("intersection", self._node, _node(o)))

    def translate(self, x=0 * mm, y=0 * mm, z=0 * mm) -> "Solid":
        return transform(self, tx=x, ty=y, tz=z)

    def rotate(self, x=0 * deg, y=0 * deg, z=0 * deg) -> "Solid":
        """Rotate about the origin: first X, then Y, then Z."""
        return transform(self, rx=x, ry=y, rz=z)

    def fillet(self, r=1 * mm, edges: str = "all") -> "Solid":
        return fillet(self, r, edges)

    def shell(self, t=1 * mm, faces: str = "top") -> "Solid":
        return shell(self, t, faces)

    def chamfer(self, d=None, edges: str = None, top=None, bottom=None) -> "Solid":
        """Cylinders: chamfer(top=0.6*mm) (exact, mesh kernel). Any solid:
        chamfer(0.5*mm, edges="top") (B-Rep kernel)."""
        if d is not None or edges is not None:
            return chamfer_edges(self, d if d is not None else 0.5 * mm, edges or "all")
        if self._node.kind != "Cylinder":
            raise TypeError(f"use chamfer(d, edges=...) for a {self._node.kind}")
        return Solid(_a.with_params(self._node, "", _params("cylinder", {"chamfer_top": top, "chamfer_bottom": bottom})))

    def with_params(self, **kw) -> "Solid":
        kind = self._node.kind.lower()
        return Solid(_a.with_params(self._node, "", _params(kind, kw)))

    @property
    def kind(self) -> str:
        return self._node.kind

    def info(self, chord=None) -> dict:
        return info(self, chord=chord)

    def __repr__(self):
        return self._node.to_python()


class Obj:
    """Handle to a named object in the document (returned by show())."""

    __slots__ = ("name",)

    def __init__(self, name: str):
        self.name = name

    @property
    def solid(self) -> Solid:
        return Solid(_a.get(self.name))

    def __sub__(self, o):
        return self.solid - o

    def __or__(self, o):
        return self.solid | o

    __add__ = __or__

    def __and__(self, o):
        return self.solid & o

    def translate(self, *a, **k):
        return self.solid.translate(*a, **k)

    def rotate(self, *a, **k):
        return self.solid.rotate(*a, **k)

    def __repr__(self):
        return f"get({self.name!r})"


# --------------------------------------------------------------------------- primitives

def box(x=10 * mm, y=10 * mm, z=10 * mm) -> Solid:
    """Box centered on X/Y, standing on z = 0."""
    return Solid(_a.make_primitive("box", _params("box", {"x": x, "y": y, "z": z})))


def cube(s=10 * mm) -> Solid:
    return box(s, s, s)


def cylinder(d=None, h=10 * mm, *, r=None, d2=None, chamfer_top=None, chamfer_bottom=None) -> Solid:
    """Cylinder on z = 0, axis +Z. Give the diameter `d` (or radius `r`)."""
    if r is not None:
        if d is not None:
            raise TypeError("cylinder(): give d or r, not both")
        d = 2 * r if isinstance(r, Quantity) else f"2*({r})"
    if d is None:
        d = 20 * mm
    return Solid(_a.make_primitive("cylinder", _params("cylinder", {
        "d": d, "h": h, "d2": d2, "chamfer_top": chamfer_top, "chamfer_bottom": chamfer_bottom})))


def cone(d=20 * mm, d2=0 * mm, h=10 * mm) -> Solid:
    return cylinder(d=d, h=h, d2=d2)


def sphere(d=None, *, r=None) -> Solid:
    """Sphere centered at the origin."""
    if r is not None:
        d = 2 * r if isinstance(r, Quantity) else f"2*({r})"
    if d is None:
        d = 20 * mm
    return Solid(_a.make_primitive("sphere", _params("sphere", {"d": d})))


def torus(R=20 * mm, r=5 * mm) -> Solid:
    """Torus around the Z axis, centered at the origin."""
    return Solid(_a.make_primitive("torus", _params("torus", {"R": R, "r": r})))


def transform(s, tx=None, ty=None, tz=None, rx=None, ry=None, rz=None) -> Solid:
    """Rotate (X, then Y, then Z, about the origin), then translate."""
    p = _params("transform", {"tx": tx, "ty": ty, "tz": tz, "rx": rx, "ry": ry, "rz": rz})
    return Solid(_a.make_transform(_node(s), p))


# --------------------------------------------------------------------------- M2 features

def _sk(x):
    from .sketch import Sketch
    if isinstance(x, Sketch):
        return x._node()
    return _node(x)


def _flag(v) -> tuple:
    return (1.0 if v else 0.0, "")


def extrude(sketch, distance=10 * mm, symmetric: bool = False) -> Solid:
    """Extrudes the closed profiles of a sketch along its normal (holes stay holes)."""
    p = _params("extrude", {"distance": distance})
    p["symmetric"] = _flag(symmetric)
    return Solid(_a.make_feature("extrude", [_sk(sketch)], p, {}))


def revolve(sketch, angle=360 * deg, axis: str = "v") -> Solid:
    """Revolves the profiles about the sketch's horizontal ('u') or vertical ('v') axis."""
    if axis not in ("u", "v"):
        raise ValueError("revolve(axis=...): 'u' (horizontal) or 'v' (vertical)")
    p = _params("revolve", {"angle": angle})
    p["axis"] = (1.0 if axis == "v" else 0.0, "")
    return Solid(_a.make_feature("revolve", [_sk(sketch)], p, {}))


def fillet(solid, r=1 * mm, edges: str = "all") -> Solid:
    """Rounds edges. edges: 'all', 'top', 'bottom', 'vertical', 'horizontal' or
    'near:x,y,z;x,y,z' (the edge closest to each point, in mm)."""
    return Solid(_a.make_feature("fillet", [_node(solid)], _params("fillet", {"r": r}), {"edges": edges}))


def chamfer_edges(solid, d=0.5 * mm, edges: str = "all") -> Solid:
    return Solid(_a.make_feature("chamfer", [_node(solid)], _params("chamfer", {"d": d}), {"edges": edges}))


def shell(solid, t=1 * mm, faces: str = "top") -> Solid:
    """Hollows a solid with wall thickness t, opening the 'top'/'bottom' faces ('none': closed)."""
    return Solid(_a.make_feature("shell", [_node(solid)], _params("shell", {"t": t}), {"faces": faces}))


def pattern_linear(solid, n: int = 3, dx=10 * mm, dy=0 * mm, dz=0 * mm) -> Solid:
    p = _params("linear_pattern", {"dx": dx, "dy": dy, "dz": dz})
    p["n"] = (float(n), "")
    return Solid(_a.make_feature("linear_pattern", [_node(solid)], p, {}))


def pattern_circular(solid, n: int = 6, angle=360 * deg) -> Solid:
    """n copies about the Z axis, spread over `angle` (360 deg: evenly around)."""
    p = _params("circular_pattern", {"angle": angle})
    p["n"] = (float(n), "")
    return Solid(_a.make_feature("circular_pattern", [_node(solid)], p, {}))


def loft(*sketches, ruled: bool = False) -> Solid:
    """Solid through the outer profiles of two or more sketches (in order)."""
    return Solid(_a.make_feature("loft", [_sk(s) for s in sketches], {"ruled": _flag(ruled)}, {}))


def sweep(profile, path) -> Solid:
    """Sweeps a closed profile sketch along an open path sketch."""
    return Solid(_a.make_feature("sweep", [_sk(profile), _sk(path)], {}, {}))


def ref(name: str) -> Solid:
    """Parametric link to another object: follows every later change of it."""
    _a.get(name)
    return Solid(_a.make_ref(name))


def import_step_solid(path: str, sha256: str = "") -> Solid:
    import hashlib
    p = _os.path.abspath(_os.fspath(path))
    with open(p, "rb") as f:
        h = hashlib.sha256(f.read()).hexdigest()
    if sha256 and sha256 != h:
        raise ValueError(f"'{p}' changed since it was imported (sha256 {h[:12]}... != {sha256[:12]}...)")
    return Solid(_a.make_feature("step", [], {}, {"path": p, "sha256": h}))


def import_step(path: str, name: str = None) -> Obj:
    """Imports a STEP file as an exact B-Rep object (filletable, exportable)."""
    s = import_step_solid(path)
    base = _os.path.splitext(_os.path.basename(path))[0]
    base = "".join(c if c.isalnum() or c in "_-" else "_" for c in base) or "Step"
    if not base[0].isalpha():
        base = "S_" + base
    return show(s, name=name or base)


def export_step(s, path: str):
    """Exports exact geometry as STEP AP214 (unit mm)."""
    path = _os.path.abspath(_os.fspath(path))
    _a.export_step(_node(s), path)
    print(f"exported {path} (STEP, mm)")


# --------------------------------------------------------------------------- document commands

def show(s, name: str = None, visible: bool = True) -> Obj:
    """Adds the solid to the document (or replaces the geometry of `name`)."""
    _state["shown"] = True
    return Obj(_a.add_object(name or "", _node(s), bool(visible)))


def hide(*names):
    for n in names:
        _a.set_visible(_name(n), False)


def unhide(*names):
    for n in names:
        _a.set_visible(_name(n), True)


def delete(*names):
    for n in names:
        _a.remove_object(_name(n))


def rename(obj, new_name: str) -> Obj:
    _a.rename(_name(obj), new_name)
    return Obj(new_name)


def get(name: str) -> Obj:
    _a.get(name)  # raises KeyError with a precise message
    return Obj(name)


def objects() -> list:
    return [Obj(n) for n, _ in _a.objects()]


def clear():
    """Removes all objects and variables (one undoable step)."""
    _a.clear()


def var(name: str, value):
    """Defines/changes a named variable; parameters like d="2*r" follow it."""
    if isinstance(value, Quantity):
        dim = "Length" if value.dim == LENGTH else "Angle" if value.dim == ANGLE else None
        if dim is None:
            raise TypeError(f"variables must be lengths or angles, got {dim_name(value.dim)}")
        _a.set_var(name, value.value, "", dim)
    elif isinstance(value, str):
        v, dimname = _a.eval_expr(value)
        _a.set_var(name, 0.0, value, dimname)
    elif isinstance(value, Real):
        _a.set_var(name, float(value), "", "Dimensionless")
    else:
        raise TypeError("var(): value must be a quantity, a number or an expression string")
    return value


def edit(obj, path: str = "", **params):
    """Changes parameters of an object's node, e.g. edit("Base", d=26*mm) or
    edit("Base", "a", h=4*mm) for the first operand of a boolean."""
    name = _name(obj)
    node = _a.get(name).child(path) if path else _a.get(name)
    kind = node.kind.lower()
    if kind not in _PARAM_DIMS:
        raise TypeError(f"{node.kind} has no editable parameters")
    _a.edit(name, path, _params(kind, params))


def move(obj, x=0 * mm, y=0 * mm, z=0 * mm):
    """Translates an object by (x, y, z)."""
    name = _name(obj)
    node = _a.get(name)
    dx, dy, dz = (_param(k, v, LENGTH) for k, v in (("x", x), ("y", y), ("z", z)))
    p = node.params() if node.kind == "Transform" else None
    if p and all(p[k][1] == "" for k in ("tx", "ty", "tz")) and all(d[1] == "" for d in (dx, dy, dz)):
        # fold into the existing transform: translation is applied last, so adding is exact composition
        new = {k: (p[k][0] + d[0], "") for k, d in (("tx", dx), ("ty", dy), ("tz", dz))}
        _a.edit(name, "", new)
    else:
        _a.set_root(name, _a.make_transform(node, {"tx": dx, "ty": dy, "tz": dz}))


def _combine(op: str, target, tools):
    name = _name(target)
    node = _a.get(name)
    for t in tools:
        node = _a.make_boolean(op, node, _node(t))
    _a.set_root(name, node)
    for t in tools:
        if isinstance(t, (Obj, str)):
            _a.remove_object(_name(t))
    return Obj(name)


def subtract(target, *tools) -> Obj:
    """target := target - tools; the tool objects are consumed."""
    return _combine("difference", target, tools)


def union(target, *others) -> Obj:
    return _combine("union", target, others)


def intersect(target, *others) -> Obj:
    return _combine("intersection", target, others)


def duplicate(obj, name: str = None) -> Obj:
    return show(Solid(_a.get(_name(obj))), name=name)


# --------------------------------------------------------------------------- analysis / IO

def _chord(chord) -> float:
    if chord is None:
        return _a.DEFAULT_CHORD_MM
    if isinstance(chord, Quantity):
        if chord.dim != LENGTH:
            raise TypeError("chord must be a length, e.g. chord=0.01*mm")
        return chord.value
    raise TypeError(f"chord={chord!r} has no unit; write chord={chord!r}*mm")


def info(s, chord=None) -> dict:
    return _a.evaluate_info(_node(s), _chord(chord))


def check(s, chord=None) -> dict:
    """Runs all printability checks; returns the report (does not raise)."""
    return _a.check(_node(s), _chord(chord))


def _unit_symbol(unit) -> str:
    if isinstance(unit, str):
        return unit
    for sym, q in (("mm", mm), ("cm", 10 * mm), ("m", 1000 * mm), ("in", 25.4 * mm)):
        if isinstance(unit, Quantity) and unit == q:
            return sym
    raise TypeError("unit must be mm, cm, m or inch")


def export_stl(s, path: str, unit=mm, chord=None, ascii: bool = False) -> dict:
    """Exports a printable STL. Runs every check first; raises with the precise
    defects if the mesh is not printable (nothing is written then)."""
    path = _os.fspath(path)
    rep = _a.export_stl(_node(s), _os.path.abspath(path), _unit_symbol(unit), _chord(chord), bool(ascii))
    if "sha256" in rep:
        print(f"exported {path}: {rep['triangles']} triangles, unit={rep['unit']}, "
              f"volume {rep['volume']:.6g} mm^3, sha256 {rep['sha256'][:16]}...")
    return rep


def import_mesh(path: str, unit=mm, sha256: str = "") -> Solid:
    node, _, _, _ = _a.import_mesh(_os.path.abspath(_os.fspath(path)), _unit_symbol(unit), sha256)
    return Solid(node)


def import_stl(path: str, name: str = None, unit=mm) -> Obj:
    """Imports an STL file as an object (the file hash is recorded)."""
    p = _os.path.abspath(_os.fspath(path))
    node, sha, raw, dropped = _a.import_mesh(p, _unit_symbol(unit), "")
    if dropped:
        print(f"note: {dropped} zero-area triangles were dropped while welding vertices")
    base = _os.path.splitext(_os.path.basename(p))[0]
    base = "".join(c if c.isalnum() or c in "_-" else "_" for c in base) or "Mesh"
    if not base[0].isalpha():
        base = "M_" + base
    return show(Solid(node), name=name or base)


def save(path: str):
    """Saves the document as an .anvil file (a Python script)."""
    _a.save_document(_os.path.abspath(_os.fspath(path)))


def load(path: str):
    """Replaces the document with an .anvil file (one undoable step)."""
    with open(path, "r", encoding="utf-8") as f:
        src = f.read()
    first = src.split("\n", 1)[0]
    if first.startswith("# anvil-format:") and int(first.split(":")[1]) > _a.DOCUMENT_FORMAT:
        raise ValueError(f"{path} was written by a newer ANVIL (format {first.split(':')[1].strip()})")
    clear()
    ns = {}
    exec(compile(src, path, "exec"), ns)


# --------------------------------------------------------------------------- UI requests

def view(name: str = "iso", direction=None):
    """Camera: 'top', 'bottom', 'front', 'back', 'left', 'right', 'iso',
    'frame' (fit all), 'ortho', 'perspective'; or direction=(x, y, z): look
    from that direction towards the target (the 26 ViewCube directions)."""
    if direction is not None:
        d = [float(c) for c in direction]
        if len(d) != 3 or not any(d):
            raise ValueError("view(direction=...) needs a non-zero (x, y, z)")
        _a.ui_request(_json.dumps({"request": "view", "direction": d}))
        return
    valid = {"top", "bottom", "front", "back", "left", "right", "iso", "frame", "ortho", "perspective"}
    if name not in valid:
        raise ValueError(f"view({name!r}): expected one of {', '.join(sorted(valid))}")
    _a.ui_request(_json.dumps({"request": "view", "name": name}))


def theme(name: str):
    """UI theme: 'dark', 'studio' (dark controls, light scene) or 'light' (applied live)."""
    if name not in ("dark", "studio", "light"):
        raise ValueError("theme(): 'dark', 'studio' or 'light'")
    _a.ui_request(_json.dumps({"request": "theme", "name": name}))


def display(mode: str = "shaded"):
    """Viewport display: 'shaded' or 'mesh' (every triangle edge overlaid)."""
    if mode not in ("shaded", "mesh"):
        raise ValueError("display(): 'shaded' or 'mesh'")
    _a.ui_request(_json.dumps({"request": "display", "mode": mode}))


def select(*names):
    _a.ui_request(_json.dumps({"request": "select", "names": [_name(n) for n in names]}))


def set_unit(unit):
    """Display unit for all fields (does not change any stored value)."""
    _a.ui_request(_json.dumps({"request": "unit", "unit": _unit_symbol(unit)}))
