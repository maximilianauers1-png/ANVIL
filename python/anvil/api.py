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
import math
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
    "export_step", "solidify", "deform", "sketch_polyline", "sketch_rect", "sketch_circle", "extrude_sketch",
    "cut_sketch", "solidify_sketch", "revolve_sketch", "sketch_move", "sketch_dim", "sketch_constrain",
    "sketch_set", "sketch_delete", "deform_object", "cut_with", "fillet_object", "chamfer_object",
    "shell_object", "edit_preview", "view", "theme", "display", "scenery", "select", "sculpt", "sculptable", "sculpted", "set_unit", "GeometryError",
]

GeometryError = _a.GeometryError
_state = {"shown": False}  # whether show() ran in the current command (REPL convenience)

# --------------------------------------------------------------------------- params

_PARAM_DIMS = {
    "solidify": {"width": LENGTH, "height": LENGTH}, "deform": {"twist": ANGLE, "bend": ANGLE},
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
    if isinstance(x, _a.Node):
        return x
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


def solidify(sketch, width=1 * mm, height=1 * mm, ends: str = "round", join: str = "round") -> Solid:
    """Thickens every curve chain of a sketch into a wall of `width` and `height`
    (open polylines become traces, closed ones rings). ends: round|square|flat,
    join: round|miter|square. Ideal for masks, ribs, cable guides."""
    if ends not in ("round", "square", "flat") or join not in ("round", "miter", "square"):
        raise ValueError("solidify(): ends round|square|flat, join round|miter|square")
    return Solid(_a.make_feature("solidify", [_sk(sketch)], _params("solidify", {"width": width, "height": height}),
                                 {"ends": ends, "join": join}))


def deform(solid, sx=1.0, sy=1.0, sz=1.0, taper=1.0, twist=0 * deg, bend=0 * deg) -> Solid:
    """Stretch (sx, sy, sz factors), taper (top scale), twist and bend a solid."""
    p = {k: (float(v), "") for k, v in (("sx", sx), ("sy", sy), ("sz", sz), ("taper", taper))}
    p.update(_params("deform", {"twist": twist, "bend": bend}))
    return Solid(_a.make_feature("deform", [_node(solid)], p, {}))


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
    if isinstance(s, (list, tuple)):  # several objects: one combined solid
        if not s:
            raise ValueError("export_step(): nothing to export")
        node = _node(s[0])
        for o in s[1:]:
            node = _a.make_boolean("union", node, _node(o))
    else:
        node = _node(s)
    _a.export_step(node, path)
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
    kind = {"linearpattern": "linear_pattern", "circularpattern": "circular_pattern"}.get(node.kind.lower(), node.kind.lower())
    p = {}
    for k, v in params.items():  # dimensionless parameters (counts, factors) take plain numbers
        if kind in _PARAM_DIMS and k in _PARAM_DIMS[kind]:
            p[k] = _param(k, v, _PARAM_DIMS[kind][k])
        elif isinstance(v, (int, float)) and not isinstance(v, bool):
            p[k] = (float(v), "")
        elif isinstance(v, str):
            p[k] = (0.0, v)
        else:
            raise TypeError(f"{node.kind}.{k}: unsupported value {v!r}")
    _a.edit(name, path, p)


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


# --------------------------------------------------------------------------- sculpting (M3)

_BRUSHES = ("draw", "carve", "smooth", "grab", "flatten")


def _voxel_for(name: str, voxel) -> float:
    if voxel is not None:
        return _coord_mm(voxel, "voxel")
    # default 50 um, coarser for big objects so the box stays below the voxel bound
    bb = info(name)["bbox"]
    diag = math.dist(bb[0], bb[1])
    h = _a.DEFAULT_VOXEL_MM
    box_voxels = (diag / h) ** 3 / 5.196  # cube with this diagonal
    if box_voxels > 1e9:
        h *= (box_voxels / 1e9) ** (1 / 3)
    return float(f"{h:.3g}")


def _coord_mm(v, what: str) -> float:
    if isinstance(v, Quantity):
        if v.dim != LENGTH:
            raise TypeError(f"{what} must be a length")
        return v.value
    raise TypeError(f"{what} needs a unit, e.g. 0.05*mm")


def sculpted(src, voxel=0.05 * mm, strokes=()) -> Solid:
    """A solid as a sculptable distance field (sparse bricks) with strokes
    applied in order. strokes: [(brush, radius, strength, [(x, y, z), ...])]."""
    out = []
    for s in strokes:
        brush, radius, strength, path = s
        out.append((str(brush), _coord_mm(radius, "radius"), float(strength), [tuple(map(float, p)) for p in path]))
    return Solid(_a.make_sculpt(_node(src), _coord_mm(voxel, "voxel"), out))


def sculptable(obj, voxel=None) -> Obj:
    """Turns an object into a sculptable one (voxel default 0.05 mm, coarser
    for large objects). The original stays as the parametric source."""
    name = _name(obj)
    node = _a.get(name)
    if node.kind == "Sculpt":
        return Obj(name)
    _a.set_root(name, _a.make_sculpt(node, _voxel_for(name, voxel), []))
    return Obj(name)


def sculpt(obj, brush: str = "draw", path=(), radius=2 * mm, strength: float = 0.5) -> Obj:
    """One sculpt stroke (one undo step): brush draw | carve | smooth | grab |
    flatten along `path` (points in mm on or near the surface)."""
    if brush not in _BRUSHES:
        raise ValueError(f"sculpt(): brush must be one of {', '.join(_BRUSHES)}")
    if not path:
        raise ValueError("sculpt(): empty path")
    if not 0 <= strength <= 1:
        raise ValueError("sculpt(): strength must be within 0..1")
    name = _name(obj)
    sculptable(name)
    node = _a.get(name)
    voxel, strokes = node.sculpt_info()
    strokes = list(strokes) + [(brush, _coord_mm(radius, "radius"), float(strength),
                                [tuple(float(c) for c in p) for p in path])]
    _a.set_root(name, _a.make_sculpt(node.child("src"), voxel, strokes))
    return Obj(name)


def scenery(name: str = "sunset"):
    """Background scenery of the Studio theme: 'sunset', 'fjord', 'night' or 'none'."""
    if name not in ("none", "sunset", "fjord", "night"):
        raise ValueError("scenery(): 'none', 'sunset', 'fjord' or 'night'")
    _a.ui_request(_json.dumps({"request": "scenery", "name": name}))


def select(*names):
    _a.ui_request(_json.dumps({"request": "select", "names": [_name(n) for n in names]}))


def set_unit(unit):
    """Display unit for all fields (does not change any stored value)."""
    _a.ui_request(_json.dumps({"request": "unit", "unit": _unit_symbol(unit)}))


# --------------------------------------------------------------------------- one-step UI commands
# Each is exactly one undoable command, so drawing and modelling in the UI stay
# streamlined: draw -> one click -> solid.

def _new_sketch(plane, offset) -> "Sketch":
    return Sketch(plane, offset)


def sketch_polyline(points, plane: str = "XY", offset=0 * mm, closed: bool = False, name: str = None) -> Obj:
    """Draws a polyline sketch (corners shared, axis-aligned segments constrained)."""
    s = _new_sketch(plane, offset)
    s.polyline([(_c(x), _c(y)) for x, y in points], closed=closed)
    return show(s._node_solved(), name=name)


def sketch_rect(corner, opposite, plane: str = "XY", offset=0 * mm, name: str = None) -> Obj:
    s = _new_sketch(plane, offset)
    s.rect((_c(corner[0]), _c(corner[1])), (_c(opposite[0]), _c(opposite[1])))
    return show(s._node_solved(), name=name)


def sketch_circle(center, r, plane: str = "XY", offset=0 * mm, name: str = None) -> Obj:
    s = _new_sketch(plane, offset)
    s.circle((_c(center[0]), _c(center[1])), r=r)
    return show(s._node_solved(), name=name)


def _c(v) -> float:
    return v.value if isinstance(v, Quantity) else float(v)


def _sketch_of(name: str) -> "Sketch":
    node = _a.get(name)
    if node.kind != "Sketch":
        raise TypeError(f"'{name}' is not a sketch")
    return Sketch._from_node(node)


def _derived_name(prefix: str) -> str:
    return _a.unique_name(prefix)


def extrude_sketch(sketch: str, distance=5 * mm, symmetric: bool = False, name: str = None) -> Obj:
    """Extrudes a sketch object into a new solid; the sketch stays editable (and hidden)."""
    o = show(extrude(ref(sketch), distance, symmetric), name=name or _derived_name("Extrude"))
    hide(sketch)
    return o


def revolve_sketch(sketch: str, angle=360 * deg, axis: str = "v", name: str = None) -> Obj:
    o = show(revolve(ref(sketch), angle, axis), name=name or _derived_name("Revolve"))
    hide(sketch)
    return o


def solidify_sketch(sketch: str, width=1 * mm, height=1 * mm, ends: str = "round", join: str = "round",
                    name: str = None) -> Obj:
    o = show(solidify(ref(sketch), width, height, ends, join), name=name or _derived_name("Solid"))
    hide(sketch)
    return o


def _through_depth(target: str) -> Quantity:
    (x0, y0, z0), (x1, y1, z1) = info(target)["bbox"]
    return 4 * max(x1 - x0, y1 - y0, z1 - z0, 1.0) * mm


_PLANE_NORMAL = {"XY": (0, 0, 1), "XZ": (0, -1, 0), "YZ": (1, 0, 0)}


def cut_sketch(sketch: str, target: str, depth=None, mode: str = "extrude", width=1 * mm) -> Obj:
    """Cuts the sketch's profiles out of `target`.
    depth None: through everything (symmetric about the sketch plane); a depth
    cuts from the sketch plane into the material (against the plane normal,
    e.g. down from a top face). mode='solidify' cuts the curves as traces of
    `width` (masks, grooves, engravings)."""
    through = depth is None
    d = _through_depth(target) if through else depth
    if mode == "solidify":
        tool = solidify(ref(sketch), width, d)
    elif mode == "extrude":
        tool = extrude(ref(sketch), d)
    else:
        raise ValueError("cut_sketch(mode=...): 'extrude' or 'solidify'")
    nx, ny, nz = _PLANE_NORMAL[_sketch_of(sketch).plane]
    k = 0.5 if through else 1.0  # shift back along the normal: centred (through) or fully below the plane
    tool = transform(tool, tx=-nx * k * d, ty=-ny * k * d, tz=-nz * k * d)
    _a.set_root(target, _a.make_boolean("difference", _a.get(target), tool._node))
    hide(sketch)
    return Obj(target)


def cut_with(target: str, *tools: str) -> Obj:
    """target minus each tool (tools are consumed) - the radial 'Cut with...' action."""
    return subtract(target, *tools)


def sketch_move(sketch: str, point: str, x, y) -> None:
    """Drags a sketch point; the solver keeps every constraint satisfied."""
    s = _sketch_of(sketch)
    k = s._id("K")
    s.constraints[k] = ("fix", [point], _val_len(x), _val_len(y))
    s.solve()
    del s.constraints[k]
    _a.set_root(sketch, s._node())


def _val_len(v) -> tuple:
    if isinstance(v, Quantity):
        return (v.value, "")
    return (float(v), "")


def sketch_dim(sketch: str, kind: str, *refs_and_value) -> str:
    s = _sketch_of(sketch)
    k = s.dim(kind, *refs_and_value)
    s.solve()
    _a.set_root(sketch, s._node())
    return k


def sketch_constrain(sketch: str, kind: str, *refs) -> str:
    s = _sketch_of(sketch)
    k = s.fix(refs[0]) if kind == "fix" else s.constrain(kind, *refs)
    s.solve()
    _a.set_root(sketch, s._node())
    return k


def sketch_set(sketch: str, constraint: str, value) -> None:
    """Changes a dimension's value (quantity or expression); geometry follows."""
    s = _sketch_of(sketch)
    kind, refs, _, b = s.constraints[constraint]
    from .sketch import _val
    s.constraints[constraint] = (kind, refs, _val(value, kind), b)
    s.solve()
    _a.set_root(sketch, s._node())


def sketch_delete(sketch: str, *ids) -> None:
    s = _sketch_of(sketch)
    for i in ids:
        if i in s.constraints:
            del s.constraints[i]
        elif i in s.curves:
            del s.curves[i]
            s.constraints = {k: c for k, c in s.constraints.items() if i not in c[1]}
        elif i in s.points:
            dead = [c for c, v in s.curves.items() if i in v[1]]
            for c in dead:
                del s.curves[c]
            del s.points[i]
            s.constraints = {k: c for k, c in s.constraints.items() if i not in c[1] and not set(dead) & set(c[1])}
    _a.set_root(sketch, s._node())


def deform_object(name: str, **params) -> Obj:
    """Adds (or edits) a deformation of an object: sx, sy, sz, taper, twist, bend."""
    node = _a.get(name)
    if node.kind == "Deform":
        p = {}
        for k, v in params.items():
            p[k] = _params("deform", {k: v})[k] if k in ("twist", "bend") else (float(v), "")
        _a.edit(name, "", p)
    else:
        _a.set_root(name, deform(Solid(node), **params)._node)
    return Obj(name)


def fillet_object(name: str, r=1 * mm, edges: str = "all") -> Obj:
    """Rounds edges of an object in place (one undo step)."""
    _a.set_root(name, fillet(Solid(_a.get(name)), r, edges)._node)
    return Obj(name)


def chamfer_object(name: str, d=0.5 * mm, edges: str = "all") -> Obj:
    _a.set_root(name, chamfer_edges(Solid(_a.get(name)), d, edges)._node)
    return Obj(name)


def shell_object(name: str, t=1 * mm, faces: str = "top") -> Obj:
    _a.set_root(name, shell(Solid(_a.get(name)), t, faces)._node)
    return Obj(name)


def edit_preview(name: str, path: str = "", **params) -> Solid:
    """The object with changed parameters, without changing the document
    (used for live previews while a value is being dragged)."""
    node = _a.get(name).child(path) if path else _a.get(name)
    kind = {"linearpattern": "linear_pattern", "circularpattern": "circular_pattern"}.get(node.kind.lower(), node.kind.lower())
    p = {}
    for k, v in params.items():
        if kind in _PARAM_DIMS and k in _PARAM_DIMS[kind]:
            p[k] = _param(k, v, _PARAM_DIMS[kind][k])
        else:
            p[k] = (float(v), "") if not isinstance(v, str) else (0.0, v)
    root = _a.get(name)
    return Solid(_a.with_params(root, path, p))
