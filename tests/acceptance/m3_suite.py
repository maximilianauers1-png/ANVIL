# M3 acceptance (sculpting on sparse-brick SDFs), runs inside anvil-cli.
import math
import os
import tempfile
import time

from anvil import *

pi = math.pi


def close(a, b, rel, what):
    assert abs(a - b) <= rel * abs(b), f"{what}: {a} vs {b} (rel {abs(a - b) / abs(b):.2e} > {rel})"


# --- a sphere becomes sculptable: volume within the DC/voxel error ----------------
show(sphere(d=20 * mm), name="Head")
v_sphere = info("Head")["volume"]
t0 = time.perf_counter()
sculptable("Head", voxel=0.1 * mm)
v0 = info("Head")["volume"]          # evaluation is lazy: time it with the first query
t_conv = time.perf_counter() - t0
close(v0, 4 / 3 * pi * 1000, 2e-3, "sculptable sphere")   # O(h^2): 0.375 h^2 / r = 1.9e-4 + chord of the source
assert info("Head")["manifold"]

# --- draw adds, carve removes, smooth keeps the volume roughly ----------------------
t0 = time.perf_counter()
sculpt("Head", "draw", [(-3, 0, 10), (0, 1, 10), (3, 0, 10)], radius=3 * mm, strength=0.8)
v1 = info("Head")["volume"]
t_stroke = time.perf_counter() - t0
assert v1 > v0 + 1.0, (v0, v1)
sculpt("Head", "carve", [(0, -10, 0), (0, -10, 3)], radius=2 * mm, strength=1.0)
v2 = info("Head")["volume"]
assert v2 < v1 - 0.5, (v1, v2)
sculpt("Head", "smooth", [(-3, 0, 10), (3, 0, 10)], radius=3 * mm, strength=1.0)
sculpt("Head", "grab", [(10, 0, 0), (12, 0, 0)], radius=3 * mm, strength=1.0)
bb = info("Head")["bbox"]
assert bb[1][0] > 11.0, bb               # the grabbed side moved out
sculpt("Head", "flatten", [(0, 0, -10), (0, 2, -10)], radius=4 * mm, strength=1.0)

# --- export checks pass (closed, oriented, no self-intersections) -------------------
c = check("Head")
assert c["ok"], c["summary"]

# --- deterministic replay: save, load, same geometry --------------------------------
v_before = info("Head")["volume"]
with tempfile.TemporaryDirectory() as d:
    f = os.path.join(d, "head.anvil")
    save(f)
    src = open(f, encoding="utf-8").read()
    assert "sculpted(" in src and "\"grab\"" in src
    load(f)
assert info("Head")["volume"] == v_before, (info("Head")["volume"], v_before)

# --- sculpted objects combine with everything else (Manifold built on demand) -------
show(cylinder(d=4 * mm, h=30 * mm).translate(z=-15 * mm), name="Bore")
subtract("Head", "Bore")
assert info("Head")["volume"] < v_before - 200   # bore: pi 2^2 * ~20 mm = 251 mm^3
assert check("Head")["ok"]

# --- errors are precise -------------------------------------------------------------
for bad, msg in ((lambda: sculpt("Head", "pinch", [(0, 0, 0)]), "brush"),
                 (lambda: sculpt("Head", "draw", []), "empty path"),
                 (lambda: sculpted(sphere(d=3000 * mm), voxel=0.01 * mm), "too fine")):
    try:
        r = bad()
        if msg == "too fine":
            info(r)
        raise AssertionError("no error for " + msg)
    except (ValueError, GeometryError) as e:
        assert msg in str(e), e

print(f"conversion {t_conv * 1e3:.0f} ms, one stroke incl. remesh {t_stroke * 1e3:.0f} ms")
print("M3 SUITE OK")
