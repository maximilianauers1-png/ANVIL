"""Local AI assistant (M6, ADR 0019): natural language -> typed tool calls ->
reviewable ANVIL Python.

The language model (Qwen via a local Ollama server, reached only through the
anvil-ai worker on 127.0.0.1) never produces code that runs. It may only call
the tools declared here with JSON arguments; every call is validated
(object names exist and have the right kind, enums, numeric ranges) and
translated into the documented ANVIL API. The result is shown to the user as
Python and runs only after confirmation, as one undo step.

No network access here: the embedded runtime has no socket module.
"""
from __future__ import annotations

import json
import math

import _anvil as _a

# Models the installer offers (pulled by the local Ollama server). Sizes are
# Ollama's Q4_K_M downloads; all fit the 12 GB VRAM budget of ADR 0009.
MODELS = [
    {"name": "qwen2.5:7b", "label": "Qwen2.5 7B Instruct", "size_gb": 4.7, "license": "Apache-2.0",
     "source": "https://ollama.com/library/qwen2.5", "recommended": True,
     "note": "Reliable tool calling, German and English; the default"},
    {"name": "qwen3:8b", "label": "Qwen3 8B", "size_gb": 5.2, "license": "Apache-2.0",
     "source": "https://ollama.com/library/qwen3", "recommended": False,
     "note": "Newer and stronger reasoning; slower first answers"},
    {"name": "qwen2.5:3b", "label": "Qwen2.5 3B Instruct", "size_gb": 1.9, "license": "Qwen Research License (non-commercial)",
     "source": "https://ollama.com/library/qwen2.5", "recommended": False,
     "note": "Small and fast; for weak GPUs, less reliable"},
]
DEFAULT_MODEL = "qwen2.5:7b"

POSE_ANGLES = ("spine", "neck", "head_turn", "head_tilt",
               "l_shoulder_raise", "l_shoulder_fwd", "l_elbow", "r_shoulder_raise", "r_shoulder_fwd", "r_elbow",
               "l_hip_fwd", "l_hip_side", "l_knee", "r_hip_fwd", "r_hip_side", "r_knee")
PRESETS = ("stand", "walk", "attack", "cast", "guard")
WEAPONS = ("longsword", "zweihander", "buster", "dagger", "axe", "spear")
MAX_ANGLE_DEG = 180.0
MAX_LENGTH_MM = 500.0


def _obj(desc, kinds=None):
    return {"type": "string", "description": desc + (f" (kind: {', '.join(kinds)})" if kinds else "")}


def _num(desc, lo, hi):
    return {"type": "number", "description": f"{desc} ({lo} .. {hi})", "minimum": lo, "maximum": hi}


def _tool(name, desc, props, required):
    return {"type": "function", "function": {"name": name, "description": desc,
                                              "parameters": {"type": "object", "properties": props, "required": required}}}


TOOLS = [
    _tool("pose_preset", "Set a mini's whole pose from a preset.",
          {"object": _obj("imported mini", ["Model"]), "preset": {"type": "string", "enum": list(PRESETS)}},
          ["object", "preset"]),
    _tool("set_pose", "Set individual joint angles of a mini in degrees (others unchanged). "
          "Conventions: shoulder_fwd raises the arm forwards, shoulder_raise sideways, elbow/knee bend, "
          "hip_fwd lifts the leg forwards, spine bends forwards, head_turn turns left(+)/right(-). "
          "l_ = the figure's left side.",
          {"object": _obj("imported mini", ["Model"]),
           "angles": {"type": "object", "description": "joint name -> degrees",
                      "properties": {a: {"type": "number"} for a in POSE_ANGLES}}},
          ["object", "angles"]),
    _tool("set_height", "Scale a mini to a height (top of the head) in mm, e.g. 28, 32, 54.",
          {"object": _obj("imported mini", ["Model"]), "height_mm": _num("height", 5, 300)}, ["object", "height_mm"]),
    _tool("equip_weapon", "Put a weapon in a mini's hand; it follows the pose.",
          {"object": _obj("imported mini", ["Model"]), "weapon": {"type": "string", "enum": list(WEAPONS)},
           "hand": {"type": "string", "enum": ["r", "l"]},
           "pitch_deg": _num("blade up (+) / down (-) from pointing forwards", -180, 180),
           "yaw_deg": _num("sideways swing", -180, 180), "roll_deg": _num("roll about the blade", -180, 180)},
          ["object", "weapon"]),
    _tool("add_base", "Add a round miniature base (top at z = 0).",
          {"diameter_mm": _num("diameter", 10, 120), "texture": {"type": "string", "enum": ["flagstone", "rubble", "plain"]}},
          ["diameter_mm"]),
    _tool("merge_objects", "Merge objects (mini, weapons, base) into one printable object.",
          {"objects": {"type": "array", "items": {"type": "string"}, "description": "object names"}}, ["objects"]),
    _tool("slice_flat", "Cut an object horizontally at a height and keep the part above (flat bottom) or below.",
          {"object": _obj("any object"), "z_mm": _num("height of the cut", -500, 500),
           "keep": {"type": "string", "enum": ["above", "below"]}}, ["object", "z_mm"]),
    _tool("magnet_hole", "Make a cylindrical magnet hole into the underside of an object (e.g. a base).",
          {"object": _obj("any object"), "diameter_mm": _num("hole diameter (magnet + clearance)", 1, 30),
           "depth_mm": _num("depth", 0.5, 20)}, ["object", "diameter_mm", "depth_mm"]),
    _tool("move_object", "Move an object by an offset in mm (x right, y back, z up).",
          {"object": _obj("any object"), "dx_mm": _num("x", -500, 500), "dy_mm": _num("y", -500, 500), "dz_mm": _num("z", -500, 500)},
          ["object"]),
    _tool("rotate_object", "Rotate an object about the vertical axis through its centre.",
          {"object": _obj("any object"), "angle_deg": _num("angle, counter-clockwise seen from above", -360, 360)},
          ["object", "angle_deg"]),
    _tool("deformation_settings", "Change how a mini deforms when posed: weights 'auto' recomputes skin weights "
          "(use when posing stretches or tears the mesh), rest 'file' keeps the file's pose as pose 0.",
          {"object": _obj("imported mini", ["Model"]), "weights": {"type": "string", "enum": ["file", "auto"]},
           "rest": {"type": "string", "enum": ["normalize", "file"]}}, ["object"]),
    _tool("hide_objects", "Hide objects.", {"objects": {"type": "array", "items": {"type": "string"}}}, ["objects"]),
]
TOOL_NAMES = {t["function"]["name"] for t in TOOLS}


# --------------------------------------------------------------------- context
def _objects():
    out = []
    for name, visible in _a.objects():
        node = _a.get(name)
        out.append({"name": name, "kind": node.kind, "visible": bool(visible), "params": node.params()})
    return out


def context(selection=()):
    lines = []
    for o in _objects():
        s = f"- {o['name']} ({o['kind']}{'' if o['visible'] else ', hidden'})"
        if o["kind"] == "Model":
            p = o["params"]
            s += f" height {p['height'][0]:.1f} mm"
            posed = {k: round(math.degrees(p[k][0]), 1) for k in POSE_ANGLES if k in p and abs(p[k][0]) > 1e-9}
            if posed:
                s += " pose " + json.dumps(posed)
        lines.append(s)
    sel = ", ".join(selection) if selection else "nothing"
    return "\n".join(lines) if lines else "(empty scene)", sel


SYSTEM = """You are the assistant inside ANVIL, a local CAD program for 3D-printed tabletop miniatures.
You change the scene ONLY by calling the provided tools; never write code. Units: mm and degrees.
Frame: Z up, figures face -Y (towards the viewer), the figure's left side is +X.
If a request cannot be done with the tools, answer in one or two short sentences (in the user's language).
Use exact object names from the scene. If the user says "it"/"the mini" use the selected object, else the only mini.
Scene objects:
{objects}
Selected: {selection}"""


def build_request(text: str, model: str = DEFAULT_MODEL, selection=()) -> dict:
    objs, sel = context(selection)
    return {
        "model": model or DEFAULT_MODEL,
        "messages": [{"role": "system", "content": SYSTEM.format(objects=objs, selection=sel)},
                     {"role": "user", "content": text}],
        "tools": TOOLS,
        "options": {"temperature": 0.1, "num_ctx": 8192},
        "keep_alive": "15m",
    }


# --------------------------------------------------------------------- mapping
class ToolError(ValueError):
    pass


def _names():
    return {o["name"]: o["kind"] for o in _objects()}


def _need(args, key):
    if key not in args:
        raise ToolError(f"missing argument '{key}'")
    return args[key]


def _object(args, key="object", kinds=None):
    names = _names()
    n = _need(args, key)
    if not isinstance(n, str) or n not in names:
        raise ToolError(f"no object named {n!r} (scene: {', '.join(sorted(names)) or 'empty'})")
    if kinds and names[n] not in kinds:
        raise ToolError(f"{n!r} is a {names[n]}, not {' / '.join(kinds)}")
    return n


def _number(args, key, lo, hi, default=None):
    v = args.get(key, default)
    if v is None:
        raise ToolError(f"missing argument '{key}'")
    try:
        v = float(v)
    except (TypeError, ValueError):
        raise ToolError(f"{key} must be a number, got {v!r}") from None
    if not (lo <= v <= hi) or not math.isfinite(v):
        raise ToolError(f"{key} = {v:g} is outside {lo:g} .. {hi:g}")
    return v


def _enum(args, key, values, default=None):
    v = args.get(key, default)
    if v not in values:
        raise ToolError(f"{key} must be one of {', '.join(values)}, got {v!r}")
    return v


def _q(v):  # number literal without float noise
    return repr(round(float(v), 6))


def _map_call(name, args):
    """One tool call -> (python source line(s), short summary)."""
    if not isinstance(args, dict):
        raise ToolError("arguments must be an object")
    if name == "pose_preset":
        o = _object(args, kinds=("Model", "Figure"))
        p = _enum(args, "preset", PRESETS)
        return f"pose({o!r}, {p!r})", f"Pose {o}: {p}"
    if name == "set_pose":
        o = _object(args, kinds=("Model", "Figure"))
        ang = _need(args, "angles")
        if not isinstance(ang, dict) or not ang:
            raise ToolError("angles must be an object of joint -> degrees")
        parts = []
        for k, v in ang.items():
            if k not in POSE_ANGLES:
                raise ToolError(f"unknown joint {k!r} (known: {', '.join(POSE_ANGLES)})")
            parts.append(f"{k}={_q(_number(ang, k, -MAX_ANGLE_DEG, MAX_ANGLE_DEG))}*deg")
        return f"edit({o!r}, {', '.join(parts)})", f"{o}: " + ", ".join(f"{k} {float(v):g}°" for k, v in ang.items())
    if name == "set_height":
        o = _object(args, kinds=("Model", "Figure"))
        h = _number(args, "height_mm", 5, 300)
        return f"edit({o!r}, height={_q(h)}*mm)", f"{o}: height {h:g} mm"
    if name == "equip_weapon":
        o = _object(args, kinds=("Model",))
        w = _enum(args, "weapon", WEAPONS)
        hand = _enum(args, "hand", ("r", "l"), "r")
        extra = ""
        for k, kw in (("pitch_deg", "pitch"), ("yaw_deg", "yaw"), ("roll_deg", "roll")):
            if k in args:
                extra += f", {kw}={_q(_number(args, k, -180, 180))}*deg"
        return f"equip({o!r}, {w!r}, hand={hand!r}{extra})", f"{o}: {w} in the {'right' if hand == 'r' else 'left'} hand"
    if name == "add_base":
        d = _number(args, "diameter_mm", 10, 120)
        t = _enum(args, "texture", ("flagstone", "rubble", "plain"), "flagstone")
        return f"show(mini_base(d={_q(d)}*mm, texture={t!r}), name=unique_name('Base'))", f"Base {d:g} mm ({t})"
    if name == "merge_objects":
        objs = _need(args, "objects")
        if not isinstance(objs, list) or len(objs) < 2:
            raise ToolError("merge needs at least two objects")
        names = [_object({"o": n}, "o") for n in objs]
        return f"merge({', '.join(repr(n) for n in names)})", "Merge " + " + ".join(names)
    if name == "slice_flat":
        o = _object(args)
        z = _number(args, "z_mm", -MAX_LENGTH_MM, MAX_LENGTH_MM)
        keep = _enum(args, "keep", ("above", "below"), "above")
        return f"slice_cut({o!r}, (0, 0, {_q(z)}), keep={keep!r})", f"{o}: cut at z = {z:g} mm, keep {keep}"
    if name == "magnet_hole":
        o = _object(args)
        d = _number(args, "diameter_mm", 1, 30)
        h = _number(args, "depth_mm", 0.5, 20)
        return f"magnet_hole({o!r}, d={_q(d)}*mm, depth={_q(h)}*mm)", f"{o}: magnet hole Ø{d:g} x {h:g} mm"
    if name == "move_object":
        o = _object(args)
        dx, dy, dz = (_number(args, k, -MAX_LENGTH_MM, MAX_LENGTH_MM, 0) for k in ("dx_mm", "dy_mm", "dz_mm"))
        if dx == dy == dz == 0:
            raise ToolError("move by zero")
        return f"move({o!r}, x={_q(dx)}*mm, y={_q(dy)}*mm, z={_q(dz)}*mm)", f"Move {o} by ({dx:g}, {dy:g}, {dz:g}) mm"
    if name == "rotate_object":
        o = _object(args)
        a = _number(args, "angle_deg", -360, 360)
        return f"rotate({o!r}, rz={_q(a)}*deg)", f"Rotate {o} by {a:g}°"
    if name == "deformation_settings":
        o = _object(args, kinds=("Model",))
        kw = []
        if "weights" in args:
            kw.append(f"weights={_enum(args, 'weights', ('file', 'auto'))!r}")
        if "rest" in args:
            kw.append(f"rest={_enum(args, 'rest', ('normalize', 'file'))!r}")
        if not kw:
            raise ToolError("nothing to change")
        return f"model_options({o!r}, {', '.join(kw)})", f"{o}: deformation " + ", ".join(kw)
    if name == "hide_objects":
        objs = _need(args, "objects")
        if not isinstance(objs, list) or not objs:
            raise ToolError("objects must be a non-empty list")
        names = [_object({"o": n}, "o") for n in objs]
        return f"hide({', '.join(repr(n) for n in names)})", "Hide " + ", ".join(names)
    raise ToolError(f"unknown tool {name!r}")


def map_reply(response: dict) -> dict:
    """Ollama /api/chat response -> {"python", "summary", "answer", "errors"}."""
    msg = (response or {}).get("message") or {}
    lines, summary, errors = [], [], []
    for call in msg.get("tool_calls") or []:
        fn = (call or {}).get("function") or {}
        name = fn.get("name", "")
        args = fn.get("arguments", {})
        if isinstance(args, str):  # some models send the arguments as a JSON string
            try:
                args = json.loads(args)
            except ValueError:
                errors.append(f"{name}: arguments are not valid JSON")
                continue
        try:
            src, text = _map_call(name, args)
            lines.append(src)
            summary.append(text)
        except ToolError as e:
            errors.append(f"{name}: {e}")
    answer = (msg.get("content") or "").strip()
    # strip a reasoning block some models (qwen3) prepend
    if answer.startswith("<think>") and "</think>" in answer:
        answer = answer.split("</think>", 1)[1].strip()
    return {"python": "\n".join(lines), "summary": summary, "answer": answer, "errors": errors}


def models() -> list:
    return MODELS
