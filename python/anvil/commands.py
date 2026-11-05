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


# Radial layout: slot index = sector (0 = east, counter-clockwise in 45 degree steps).
# Positions are stable across contexts: inapplicable commands are greyed out,
# never removed or reordered (muscle memory).
_VIEW = {"submenu": "View", "items": ["view.right", "view.iso", "view.top", "view.back", "view.left",
                                      "view.ortho", "view.front", "view.frame"]}
registry.radial = {
    "Design": {
        "empty": ["create.cylinder", "create.sphere", "create.box", "create.torus", "file.import_stl",
                  "create.cone", "file.export_stl", _VIEW],
        "selection": ["edit.move", "boolean.union", "boolean.subtract", "boolean.intersect", "edit.delete",
                      "edit.hide", "file.export_stl", _VIEW],
    },
}
