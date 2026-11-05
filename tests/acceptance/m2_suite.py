# M2 acceptance, runs inside anvil-cli (B-Rep kernel in the anvil-geom worker).
from anvil import *
import _anvil
import math
import os
import tempfile

pi = math.pi


def close(a, b, rel, what):
    assert abs(a - b) <= rel * abs(b), f"{what}: {a} vs {b} (rel {abs(a - b) / abs(b):.2e} > {rel})"


def vol(s, chord=0.002 * mm):
    i = info(s, chord=chord)
    assert i["manifold"], i
    return i["volume"]


# --- extrude with a hole: V = (w h - pi r^2) d -------------------------------------
s = Sketch("XY")
a, b, c, d = s.rect((0, 0), (40, 20))
s.circle((20, 10), r=3 * mm)
close(vol(extrude(s, 5 * mm)), (800 - pi * 9) * 5, 2e-4, "extrude")
close(vol(extrude(s, 5 * mm, symmetric=True)), (800 - pi * 9) * 5, 2e-4, "symmetric extrude")
assert abs(info(extrude(s, 5 * mm, symmetric=True))["bbox"][0][2] + 2.5) < 1e-9

# --- revolve a rectangle about the sketch's vertical axis: cylinder ----------------
r = Sketch("XZ")
r.rect((0, 0), (10, 4))
close(vol(revolve(r, axis="v")), pi * 100 * 4, 2e-4, "revolve 360")
close(vol(revolve(r, 90 * deg, axis="v")), pi * 100 * 4 / 4, 2e-4, "revolve 90")

# --- fillet: each rounded edge removes (1 - pi/4) r^2 L ------------------------------
bx = Sketch("XY")
bx.rect((0, 0), (20, 20))
blk = extrude(bx, 10 * mm)
close(vol(blk.fillet(2 * mm, edges="vertical")), 4000 - 4 * (1 - pi / 4) * 4 * 10, 2e-4, "fillet")
assert check(blk.fillet(1 * mm, edges="all"))["ok"]

# --- shell open at the top: V = outer - inner ------------------------------------------
close(vol(shell(blk, 1 * mm, faces="top")), 4000 - 18 * 18 * 9, 1e-6, "shell")

# --- chamfer on a box edge: removes d^2/2 * L ---------------------------------------------
close(vol(chamfer_edges(blk, 1 * mm, edges="near:10,10,10")), 4000 - 0.5 * 1 * 20, 1e-6, "chamfer one edge")

# --- patterns (mesh kernel and B-Rep kernel agree) ----------------------------------------
pin = cylinder(d=2 * mm, h=5 * mm)
v1 = vol(pin)
close(vol(pattern_linear(pin, 4, dx=5 * mm)), 4 * v1, 1e-9, "linear pattern (mesh kernel)")
close(vol(pattern_circular(pin.translate(x=10 * mm), 6)), 6 * v1, 1e-9, "circular pattern")
close(vol(pattern_linear(blk.fillet(1 * mm, edges="vertical"), 3, dx=30 * mm)),
      3 * vol(blk.fillet(1 * mm, edges="vertical")), 1e-6, "linear pattern (B-Rep)")

# --- loft between two squares: frustum V = h/3 (A1 + A2 + sqrt(A1 A2)) --------------------
s1 = Sketch("XY"); s1.rect((-10, -10), (10, 10))
s2 = Sketch("XY", offset=10 * mm); s2.rect((-5, -5), (5, 5))
close(vol(loft(s1, s2, ruled=True)), 10 / 3 * (400 + 100 + 200), 1e-9, "loft (ruled)")

# --- sweep a circle along a straight path = cylinder ------------------------------------------
prof = Sketch("XY"); prof.circle((0, 0), r=2 * mm)
path = Sketch("XZ"); path.line((0, 0), (0, 30))
close(vol(sweep(prof, path)), pi * 4 * 30, 4 / 3 * 0.002 / 2, "sweep")  # tessellation bound (4/3) delta/r

# --- STEP round trip -------------------------------------------------------------------------------
part = blk.fillet(2 * mm, edges="vertical") - cylinder(d=4 * mm, h=10 * mm).translate(x=10 * mm, y=10 * mm)
with tempfile.TemporaryDirectory() as d:
    f = os.path.join(d, "part.step")
    export_step(part, f)
    close(vol(import_step_solid(f)), vol(part), 1e-9, "STEP round trip")

# --- crash isolation: the worker dies, the session continues --------------------------------
starts = _anvil.geom_starts()
_anvil.geom_debug_crash()
close(vol(blk.fillet(1.5 * mm, edges="vertical")), 4000 - 4 * (1 - pi / 4) * 2.25 * 10, 2e-4, "after crash")
assert _anvil.geom_starts() == starts + 1, "worker was not restarted"

# --- precise errors -------------------------------------------------------------------------------------
try:
    vol(blk.fillet(15 * mm, edges="vertical"))
    raise AssertionError("expected a fillet failure")
except GeometryError as e:
    assert "fillet" in str(e), e
o = Sketch("XY"); o.line((0, 0), (10, 0)); o.line((10, 0), (10, 5))
try:
    vol(extrude(o, 1 * mm))
    raise AssertionError("expected an open-profile error")
except GeometryError as e:
    assert "profile is open at" in str(e), e

# --- M2 acceptance: parametric clamp for thin polymer films -------------------------------------
# A 40 mm long block of width w with a horizontal slot of height `film` at z = 4 mm
# (the film is clamped in the slot) and two M3 clearance holes, vertical edges rounded.
var("w", 30 * mm)
var("film", 0.5 * mm)
var("hole_d", 3.4 * mm)
base = Sketch("XY")
base.rect((0, -15), (40, 15))
base.dim("length", "L2", "w")            # width follows w (L2 is the right side)
base.dim("hdistance", "P1", "P2", 40 * mm)
base.fix("P1", 0 * mm, "-w/2")
show(extrude(base, 8 * mm), name="ClampBody")
slot = box(x=50 * mm, y="w + 2 mm", z="film").translate(x=20 * mm, z=4 * mm)
holes = (cylinder(d="hole_d", h=20 * mm).translate(x=8 * mm, z=-5 * mm)
         | cylinder(d="hole_d", h=20 * mm).translate(x=32 * mm, z=-5 * mm))
show(fillet(ref("ClampBody") - slot - holes, 1 * mm, edges="vertical"), name="Clamp")


def clamp_volume():
    return info("Clamp", chord=0.002 * mm)["volume"]


v_a = clamp_volume()
bb = info("Clamp")["bbox"]
assert abs((bb[1][1] - bb[0][1]) - 30) < 1e-6, bb
var("film", 1.0 * mm)
v_b = clamp_volume()
# d V / d film = -(40 * w) + hole correction (holes no longer remove material inside the slot)
# + fillet correction (vertical edges get shorter by the slot height)
r_h = 1.7
expected = -40 * 30 * 0.5 + 2 * pi * r_h ** 2 * 0.5 + 4 * (1 - pi / 4) * 1 * 0.5
close(v_b - v_a, expected, 2e-3, "clamp: film change propagates")
var("w", 35 * mm)
bb = info("Clamp")["bbox"]
assert abs((bb[1][1] - bb[0][1]) - 35) < 1e-6, f"clamp: width change propagates through the sketch, bbox {bb}"
assert check("Clamp")["ok"], check("Clamp")["summary"]
# --- streamlined one-step commands: polyline/solidify, masks, deform, dragging --------------
show(box(x=40 * mm, y=30 * mm, z=1 * mm), name="Mask")
sketch_polyline([(-15, -10), (-5, -10), (-5, 5), (10, 5), (10, 12)], plane="XY", offset=1 * mm, name="Trace")
v0 = info("Mask")["volume"]
cut_sketch("Trace", "Mask", depth=0.2 * mm, mode="solidify", width=0.5 * mm)
removed = v0 - info("Mask")["volume"]
# straight parts + round end caps; joins overlap, so the area lies between these bounds
L = 10 + 15 + 15 + 7
assert 0.2 * (0.5 * L) < removed < 0.2 * (0.5 * L + pi * 0.25 ** 2) + 1e-9, removed
assert check("Mask")["ok"]
sketch_circle((0, 0), r=10 * mm, name="Ring")
solidify_sketch("Ring", width=2 * mm, height=5 * mm, name="Wall")
close(info("Wall", chord=0.002 * mm)["volume"], pi * (11 ** 2 - 9 ** 2) * 5, 1e-3, "solidify ring")
v_wall = info("Wall", chord=0.002 * mm)["volume"]
deform_object("Wall", taper=0.7)
close(info("Wall", chord=0.002 * mm)["volume"], v_wall * (1 - 0.3 + 0.09 / 3), 2e-3, "taper: V factor = int (1-0.3t)^2 dt")
deform_object("Wall", twist=45 * deg)
assert check("Wall")["ok"]
sketch_rect((0, 0), (20, 10), name="R")
sketch_move("R", "P3", 25 * mm, 12 * mm)
extrude_sketch("R", 3 * mm, name="Dragged")
close(info("Dragged")["volume"], 25 * 12 * 3, 1e-9, "dragged corner keeps the rectangle")
k = sketch_dim("R", "length", "L1", "30 mm")
close(info("Dragged")["volume"], 30 * 12 * 3, 1e-9, "dimension edits propagate to the extrusion")
print("M2 SUITE OK")
