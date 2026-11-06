"""Command registry: the single list of user-facing commands.

Each command turns a UI context (selection, snapped point, ...) into Python
source. The UI shows the same registry in the Command Bar (fuzzy search) and
the radial menu; plugins add their own commands with `registry.add(...)`.
Executing the generated source goes through the command bus, so every UI
action is a Python command and an undo step.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from typing import Callable, Optional

__all__ = ["Command", "Context", "registry", "command"]


@dataclass
class Context:
    selection: list = field(default_factory=list)  # object names
    point: Optional[list] = None                    # snapped world point [x, y, z] in mm
    mode: str = "Design"


@dataclass
class Command:
    id: str
    label: str
    category: str
    help: str
    build: Callable[[Context], Optional[str]]  # returns Python source, or None if not applicable
    min_selection: int = 0
    max_selection: int = 1 << 30
    keywords: str = ""
    ui: str = ""  # non-empty: handled by the UI before generating Python (e.g. "export_dialog", "move_tool")

    def applicable(self, ctx: Context) -> bool:
        return self.min_selection <= len(ctx.selection) <= self.max_selection


class Registry:
    def __init__(self):
        self._cmds: dict = {}
        self.radial: dict = {}  # mode -> context -> [8 slots], slot = command id | {"submenu": label, "items": [...]}

    def add(self, cmd: Command):
        if cmd.id in self._cmds:
            raise ValueError(f"command '{cmd.id}' is already registered")
        self._cmds[cmd.id] = cmd
        return cmd

    def get(self, cid: str) -> Command:
        return self._cmds[cid]

    def all(self):
        return list(self._cmds.values())

    def build(self, cid: str, ctx: dict) -> Optional[str]:
        c = self._cmds[cid]
        cx = Context(**ctx)
        if not c.applicable(cx):
            raise ValueError(f"'{c.label}' needs {c.min_selection}..{c.max_selection} selected objects")
        return c.build(cx)

    def to_json(self) -> str:
        return json.dumps({
            "commands": [{"id": c.id, "label": c.label, "category": c.category, "help": c.help,
                          "min": c.min_selection, "max": min(c.max_selection, 1 << 20), "keywords": c.keywords,
                          "ui": c.ui} for c in self._cmds.values()],
            "radial": self.radial,
        })


registry = Registry()


def command(id, label, category, help, **kw):
    def deco(fn):
        registry.add(Command(id=id, label=label, category=category, help=help, build=fn, **kw))
        return fn
    return deco


# --------------------------------------------------------------------------- helpers

def _lit(x: float) -> str:
    return f"{x!r}*mm"


def _at(src: str, ctx: Context) -> str:
    p = ctx.point
    if p and any(abs(v) > 0 for v in p):
        args = ", ".join(f"{a}={_lit(v)}" for a, v in zip("xyz", p) if v != 0)
        src = f"{src}.translate({args})"
    return f"show({src})"


def _names(ctx: Context) -> str:
    return ", ".join(repr(n) for n in ctx.selection)


# --------------------------------------------------------------------------- built-ins

@command("app.free_memory", "Free memory (clear caches)", "View",
         "Empty the geometry caches; the document and undo history stay; results are recomputed when needed",
         keywords="ram memory cache clear purge")
def _(ctx):
    return "print(free_memory())"


@command("create.cylinder", "Cylinder", "Create", "Cylinder on the snapped point (d 20 mm, h 10 mm)",
         keywords="primitive round disc base")
def _(ctx):
    return _at("cylinder(d=20*mm, h=10*mm)", ctx)


@command("create.box", "Box", "Create", "Box on the snapped point (20 x 20 x 20 mm)", keywords="cube primitive")
def _(ctx):
    return _at("box(x=20*mm, y=20*mm, z=20*mm)", ctx)


@command("create.sphere", "Sphere", "Create", "Sphere centered on the snapped point (d 20 mm)", keywords="ball")
def _(ctx):
    return _at("sphere(d=20*mm)", ctx)


@command("create.torus", "Torus", "Create", "Torus centered on the snapped point (R 20 mm, r 5 mm)",
         keywords="ring donut")
def _(ctx):
    return _at("torus(R=20*mm, r=5*mm)", ctx)


@command("create.cone", "Cone", "Create", "Cone on the snapped point (d 20 mm, h 20 mm)", keywords="spike horn")
def _(ctx):
    return _at("cone(d=20*mm, d2=0*mm, h=20*mm)", ctx)


for _id, _label, _src, _kw in (
        ("tube", "Tube", "tube(d=20*mm, d_in=16*mm, h=10*mm)", "pipe ring sleeve hollow"),
        ("prism", "Hex prism", "prism(n=6, d=20*mm, h=10*mm)", "hexagon nut polygon prism"),
        ("pyramid", "Pyramid", "pyramid(n=4, d=20*mm, h=15*mm)", "spike obelisk"),
        ("capsule", "Capsule", "capsule(d=10*mm, h=30*mm)", "pill rod"),
        ("rounded_box", "Rounded box", "rounded_box(x=20*mm, y=20*mm, z=10*mm, r=2*mm)", "fillet block soft"),
        ("wedge", "Wedge", "wedge(x=20*mm, y=10*mm, z=10*mm)", "ramp slope"),
        ("ellipsoid", "Ellipsoid", "ellipsoid(x=30*mm, y=20*mm, z=10*mm)", "egg oval"),
        ("star", "Star", "star(n=5, d=30*mm, d_in=14*mm, h=4*mm)", "emblem")):
    registry.add(Command(id=f"create.{_id}", label=_label, category="Create",
                         help=f"{_label} on the snapped point ({_src.split('(')[1].rstrip(')')})", keywords=_kw,
                         build=(lambda src: (lambda ctx: _at(src, ctx)))(_src)))


@command("boolean.subtract", "Subtract", "Boolean",
         "First selected object minus the others (the others are consumed)", min_selection=2,
         keywords="difference cut minus")
def _(ctx):
    return f"subtract({_names(ctx)})"


@command("boolean.union", "Union", "Boolean", "Merge the selected objects into the first", min_selection=2,
         keywords="add merge join")
def _(ctx):
    return f"union({_names(ctx)})"


@command("boolean.intersect", "Intersect", "Boolean", "Keep only the common volume", min_selection=2,
         keywords="common and")
def _(ctx):
    return f"intersect({_names(ctx)})"


@command("edit.move", "Move", "Edit", "Move by two snapped points (base, then destination)", min_selection=1,
         ui="move_tool", keywords="translate place")
def _(ctx):
    return None  # the move tool generates move(...) itself


@command("edit.delete", "Delete", "Edit", "Delete the selected objects", min_selection=1, keywords="remove erase")
def _(ctx):
    return f"delete({_names(ctx)})"


@command("edit.duplicate", "Duplicate", "Edit", "Copy the selected object", min_selection=1, max_selection=1,
         keywords="copy clone")
def _(ctx):
    return f"duplicate({_names(ctx)})"


@command("edit.hide", "Hide", "Edit", "Hide the selected objects", min_selection=1, keywords="visibility")
def _(ctx):
    return f"hide({_names(ctx)})"


@command("edit.unhide_all", "Show all", "Edit", "Make every object visible", keywords="unhide visibility")
def _(ctx):
    return "unhide(*objects())"


@command("file.export_stl", "Export STL…", "File", "Check and export the selection (or everything) as STL",
         ui="export_dialog", keywords="print save mesh")
def _(ctx):
    return None


@command("file.import_stl", "Import STL…", "File", "Import a mesh file", ui="import_dialog", keywords="open load mesh")
def _(ctx):
    return None


@command("file.save", "Save…", "File", "Save the document (.anvil = Python script)", ui="save_dialog")
def _(ctx):
    return None


@command("file.open", "Open…", "File", "Open an .anvil document", ui="open_dialog")
def _(ctx):
    return None


for _name, _label in (("top", "Top view"), ("bottom", "Bottom view"), ("front", "Front view"),
                      ("back", "Back view"), ("left", "Left view"), ("right", "Right view"),
                      ("iso", "Isometric view"), ("frame", "Frame all"), ("ortho", "Orthographic"),
                      ("perspective", "Perspective")):
    registry.add(Command(id=f"view.{_name}", label=_label, category="View", help=f"Camera: {_label.lower()}",
                         build=(lambda n: (lambda ctx: f"view({n!r})"))(_name), keywords="camera"))


for _mode, _label in (("shaded", "Shaded view"), ("mesh", "Mesh view")):
    registry.add(Command(id=f"display.{_mode}", label=_label, category="View",
                         help=f"Viewport: {_label.lower()}", keywords="wireframe triangles display",
                         build=(lambda m: (lambda ctx: f"display({m!r})"))(_mode)))

for _sc, _label in (("sunset", "Scenery: sunset"), ("fjord", "Scenery: fjord"), ("night", "Scenery: night"),
                   ("none", "Scenery: plain")):
    registry.add(Command(id=f"display.scenery_{_sc}", label=_label, category="View",
                         help="Studio theme background", keywords="background environment sky landscape",
                         build=(lambda n: (lambda ctx: f"scenery({n!r})"))(_sc)))

# --- drawing (the viewport collects points, then generates sketch_* commands) ---
for _id, _label, _help in (("draw.polyline", "Polyline", "Click points; click the first point to close, double-click or Enter to finish"),
                           ("draw.rect", "Rectangle", "Two corners"),
                           ("draw.circle", "Circle", "Center, then a point on the circle")):
    registry.add(Command(id=_id, label=_label, category="Draw", help=_help, build=lambda ctx: None,
                         ui=_id.replace("draw.", "draw_"), keywords="sketch line polygon trace mask"))


def _single(ctx):
    return repr(ctx.selection[0])


@command("sketch.extrude", "Extrude", "Sketch", "Turn the sketch into a solid (5 mm, adjustable)", min_selection=1,
         max_selection=1, keywords="pad extrusion")
def _(ctx):
    return f"extrude_sketch({_single(ctx)}, 5*mm)"


@command("sketch.solidify", "Solidify", "Sketch", "Thicken the curves into walls/traces (1 mm wide, 1 mm high)",
         min_selection=1, max_selection=1, keywords="thicken trace wall rib")
def _(ctx):
    return f"solidify_sketch({_single(ctx)}, 1*mm, 1*mm)"


@command("sketch.cut", "Cut through", "Sketch", "Cut the profiles through the part the sketch was drawn on",
         min_selection=1, max_selection=1, ui="sketch_cut", keywords="hole pocket mask subtract")
def _(ctx):
    return None


@command("sketch.revolve", "Revolve", "Sketch", "Revolve about the sketch's vertical axis", min_selection=1,
         max_selection=1, keywords="lathe turn")
def _(ctx):
    return f"revolve_sketch({_single(ctx)})"


@command("sketch.edit", "Edit sketch", "Sketch", "Drag points, add dimensions and constraints", min_selection=1,
         max_selection=1, ui="sketch_edit", keywords="modify constraints dimensions")
def _(ctx):
    return None


@command("edit.cut_with", "Cut with…", "Boolean", "Subtract objects from the selected one: click them, then Enter",
         min_selection=1, ui="pick_tools", keywords="subtract difference minus hole mask")
def _(ctx):
    if len(ctx.selection) >= 2:
        return f"subtract({_names(ctx)})"
    return None


@command("edit.deform", "Deform", "Edit", "Stretch, taper, twist or bend (drag the values in the panel)",
         min_selection=1, max_selection=1, keywords="stretch taper twist bend scale warp")
def _(ctx):
    return f"deform_object({_single(ctx)})"


@command("edit.fillet", "Round edges", "Edit", "Fillet all edges (1 mm; edit radius and edge choice in the panel)",
         min_selection=1, max_selection=1, keywords="fillet round radius")
def _(ctx):
    return f"fillet_object({_single(ctx)}, 1*mm)"


@command("edit.chamfer", "Chamfer edges", "Edit", "Chamfer all edges (0.5 mm)", min_selection=1, max_selection=1,
         keywords="bevel")
def _(ctx):
    return f"chamfer_object({_single(ctx)}, 0.5*mm)"


@command("edit.shell", "Shell", "Edit", "Hollow out, open at the top (1 mm walls)", min_selection=1, max_selection=1,
         keywords="hollow wall thickness")
def _(ctx):
    return f"shell_object({_single(ctx)}, 1*mm)"


@command("file.import_step", "Import STEP…", "File", "Import exact CAD geometry", ui="import_step_dialog",
         keywords="open load cad")
def _(ctx):
    return None


@command("file.export_step", "Export STEP…", "File", "Export exact geometry (mm)", min_selection=1,
         ui="export_step_dialog", keywords="cad save")
def _(ctx):
    return None


# --- sculpting (M3): brushes are UI state; strokes become sculpt(...) commands ---
for _b, _label, _help in (("draw", "Draw", "Build up along the surface normal (Ctrl: carve)"),
                          ("carve", "Carve", "Dig in (Ctrl: draw)"),
                          ("clay", "Clay", "Build flat layers"),
                          ("inflate", "Inflate", "Swell along the local normal"),
                          ("crease", "Crease", "Sharp grooves"),
                          ("pinch", "Pinch", "Sharpen ridges"),
                          ("smooth", "Smooth", "Relax the surface (also: hold Shift)"),
                          ("grab", "Grab", "Pull the surface along with the pointer"),
                          ("flatten", "Flatten", "Plane off towards the first touched point"),
                          ("texture", "Texture", "Paint scales, chainmail, rivets or stone")):
    registry.add(Command(id=f"sculpt.{_b}", label=_label, category="Sculpt", help=_help, build=lambda ctx: None,
                         ui=f"sculpt_brush:{_b}", keywords="brush sculpt clay"))


@command("sculpt.make", "Make sculptable", "Sculpt", "Convert the selected object into sculptable clay (voxel 0.05 mm)",
         min_selection=1, max_selection=1, keywords="voxel clay convert remesh")
def _(ctx):
    return f"sculptable({_single(ctx)})"


# --- Mini Forge (M4) ------------------------------------------------------------
for _d in (25, 32, 40, 50):
    registry.add(Command(id=f"mini.base{_d}", label=f"Base {_d} mm", category="Mini",
                         help=f"Round {_d} mm base with flagstones", keywords="mini base plinth",
                         build=(lambda d: (lambda ctx: f"show(mini_base(d={d}*mm), name=unique_name('Base'))"))(_d)))
for _p in ("stand", "walk", "attack", "cast", "guard"):
    registry.add(Command(id=f"mini.pose_{_p}", label=f"Pose: {_p}", category="Mini",
                         help=f"Set the selected figure's pose to '{_p}' (angles stay editable)", min_selection=1,
                         max_selection=1, keywords="pose posture",
                         build=(lambda p: (lambda ctx: f"pose({ctx.selection[0]!r}, {p!r})"))(_p)))


@command("mini.import", "Import mini (GLB / OBJ)…", "Mini",
         "Import a mini from Tripo, Meshy, Blender, ... (.glb, .gltf, .obj, .stl), 32 mm tall, ready to pose",
         ui="model_import_dialog", keywords="tripo meshy gltf glb obj import model")
def _(ctx):
    return None


for _w, _label in (("longsword", "Longsword"), ("zweihander", "Zweihänder"), ("buster", "Buster sword"),
                   ("dagger", "Dagger"), ("axe", "Axe"), ("spear", "Spear")):
    for _h, _hl in (("r", "right"), ("l", "left")):
        registry.add(Command(id=f"mini.equip_{_w}_{_h}", label=f"{_label} ({_hl} hand)", category="Mini",
                             help=f"Put a {_label.lower()} in the selected imported mini's {_hl} hand (follows the pose)",
                             min_selection=1, max_selection=1, keywords="weapon sword accessory equip hand",
                             build=(lambda w, h: (lambda ctx: f"equip({ctx.selection[0]!r}, {w!r}, hand={h!r})"))(_w, _h)))


@command("mini.merge", "Merge for print", "Mini",
         "Merge the selected objects (mini, weapons, base, other parts) into one closed printable object; parts stay linked",
         min_selection=2, keywords="merge join combine print union")
def _(ctx):
    return "merge(" + ", ".join(repr(n) for n in ctx.selection) + ")"


@command("mini.on_base", "Stand on base", "Mini", "Join the selected figure and base into one clay object",
         min_selection=2, max_selection=2, keywords="join union merge base")
def _(ctx):
    return f"on_base({ctx.selection[0]!r}, {ctx.selection[1]!r})"


# Radial layout: slot index = sector (0 = east, counter-clockwise in 45 degree steps).
# Positions are stable across contexts: inapplicable commands are greyed out,
# never removed or reordered (muscle memory).
_VIEW = {"submenu": "View", "items": ["view.right", "view.iso", "view.top", "view.bottom", "view.left",
                                      "view.ortho", "view.front", "view.frame"]}
_MORE2 = {"submenu": "More", "items": ["create.pyramid", "create.wedge", "create.ellipsoid", "create.star",
                                       "file.import_stl", "file.import_step", "", ""]}
_MORE = {"submenu": "Shapes", "items": ["create.sphere", "create.tube", "create.cone", "create.prism",
                                       "create.torus", "create.capsule", "create.rounded_box", _MORE2]}
_EDIT = {"submenu": "Edit", "items": ["edit.fillet", "edit.shell", "edit.chamfer", "edit.duplicate", "edit.hide",
                                      "boolean.union", "boolean.intersect", ""]}
_BRUSHES_MORE = {"submenu": "Brushes", "items": ["sculpt.inflate", "sculpt.crease", "sculpt.pinch", "sculpt.flatten",
                                                "sculpt.texture", "", "", ""]}
_WEAPONS = {"submenu": "Weapon", "items": ["mini.equip_longsword_r", "mini.equip_zweihander_r", "mini.equip_buster_r",
                                          "mini.equip_dagger_l", "mini.equip_axe_r", "mini.equip_spear_r",
                                          "mini.equip_longsword_l", "mini.on_base"]}
registry.radial = {
    "Design": {
        # most used first and stable: drawing tools sit on the top half
        "empty": ["create.cylinder", "create.box", "draw.polyline", "draw.rect", "draw.circle", _MORE,
                  "file.export_stl", _VIEW],
        "selection": ["edit.move", _EDIT, "edit.cut_with", "edit.deform", "edit.delete", "boolean.union",
                      "file.export_stl", _VIEW],
        "sketch": ["sketch.extrude", "sketch.solidify", "sketch.cut", "sketch.revolve", "edit.delete",
                   "sketch.edit", "draw.polyline", _VIEW],
    },
    # Mini mode: import minis, bases, pose, weapons, merge
    "Mini": {
        "empty": ["mini.import", "file.import_stl", "mini.base32", "mini.base25", "mini.base40", "mini.base50",
                  "file.export_stl", _VIEW],
        "selection": ["mini.pose_attack", "mini.pose_walk", "mini.pose_stand", "mini.pose_cast", _WEAPONS,
                      "mini.merge", "file.export_stl", _VIEW],
    },
    # Sculpt mode: brushes always in the same places, with or without selection
    "Sculpt": {
        "empty": ["sculpt.draw", "sculpt.clay", "sculpt.smooth", "sculpt.grab", "sculpt.carve", _BRUSHES_MORE,
                  "file.export_stl", _VIEW],
        "selection": ["sculpt.draw", "sculpt.clay", "sculpt.smooth", "sculpt.grab", "sculpt.carve", _BRUSHES_MORE,
                      "sculpt.make", _VIEW],
    },
}
