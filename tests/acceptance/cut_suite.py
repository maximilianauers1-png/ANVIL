"""Cut tools: knife, lasso, drill, slice on exact solids, clay and imported minis.

Run: anvil-cli tests/acceptance/cut_suite.py   (prints "CUT SUITE OK")
"""
import math
import os
import sys
import tempfile

sys.path.insert(0, os.environ.get("ANVIL_TEST_DIR", os.path.join(os.getcwd(), "tests", "acceptance")))
from glb_fixture import humanoid_glb  # noqa: E402

import _anvil  # noqa: E402
from anvil import *  # noqa: E402,F401,F403


import time
_t0 = [time.time()]


def ok(cond, msg):
    print(f"    [{time.time() - _t0[0]:.1f} s]")
    _t0[0] = time.time()
    if not cond:
        raise AssertionError(msg)
    print("  ok:", msg)


def close(a, b, tol, what):
    ok(abs(a - b) <= tol, f"{what}: {a:.6f} vs {b:.6f}")


print("1. exact solid: knife halves, both sides add up, cut face closed")
show(box(x=20 * mm, y=20 * mm, z=10 * mm), name="B")
knife("B", [(0, -30, 5), (0, 30, 5)], direction=(0, 0, -1), keep="right")
close(info("B")["volume"], 2000, 1e-9, "knife keeps half")
ok(check("B")["ok"], "closed after the cut")
show(box(x=20 * mm, y=20 * mm, z=10 * mm), name="C")
p = knife("C", [(-30, -30, 5), (30, 30, 5)], keep="both")
close(info("C")["volume"] + info(p)["volume"], 4000, 1e-6, "keep both: the parts add up")
ok(check("C")["ok"] and check(p)["ok"], "both parts closed")

print("2. lasso (parallel and perspective), drill, slice")
show(box(x=20 * mm, y=20 * mm, z=10 * mm), name="L")
lasso_cut("L", [(-2, -2, 10), (2, -2, 10), (2, 2, 10), (-2, 2, 10)], direction=(0, 0, -1))
close(info("L")["volume"], 4000 - 160, 1e-9, "lasso removes a 4 x 4 prism")
show(box(x=20 * mm, y=20 * mm, z=10 * mm), name="P")
lasso_cut("P", [(-2, -2, 10), (2, -2, 10), (2, 2, 10), (-2, 2, 10)], direction=(0, 0, -1), eye=(0, 0, 60))
A1, A2 = 16.0, 16.0 * 1.2 ** 2
close(4000 - info("P")["volume"], 10 / 3 * (A1 + A2 + math.sqrt(A1 * A2)), 1e-6, "perspective lasso = frustum along the rays")
show(box(x=20 * mm, y=20 * mm, z=10 * mm), name="D")
drill("D", (0, 0, 10), d=3 * mm, segments=96)
n = 96
close(4000 - info("D")["volume"], 10 * 0.5 * n * 1.5 ** 2 * math.sin(2 * math.pi / n), 1e-5, "drill: 96-gon hole (points rounded to 1 nm)")
slice_cut("D", (0, 0, 2.5))
close(info("D")["bbox"][0][2], 2.5, 1e-9, "slice: flat bottom at z = 2.5")

print("3. clay and imported minis are cut in SDF space (closed result)")
show(sphere(d=20 * mm), name="S")
sculptable("S", voxel=0.2 * mm)
knife("S", [(-30, 0, 0), (30, 0, 0)], direction=(0, 0, -1), keep="left")
v = info("S")["volume"]
close(v, 4 / 3 * math.pi * 1000 / 2, 15, "clay hemisphere")
ok(check("S")["ok"], "clay cut is closed")
tmp = tempfile.mkdtemp(prefix="anvil_cut_")
m = import_model(humanoid_glb(os.path.join(tmp, "h.glb")), name="Hero")
slice_cut("Hero", (0, 0, 0.4))
c = check("Hero")
ok(c["ok"], "imported mini sliced flat: " + c["summary"])
close(info("Hero")["bbox"][0][2], 0.4, 0.06, "flat bottom of the mini at z = 0.4")

print("4. saved documents reproduce the cuts")
doc = os.path.join(tmp, "cuts.anvil")
vol = {n: info(n)["volume"] for n in ("B", "C", "P", "D")}
save(doc)
load(doc)
for n_, v_ in vol.items():
    close(info(n_)["volume"], v_, 1e-9, f"{n_} after reload")

print("5. precise errors")
for bad, want in ((lambda: info(knife("B", [(1, 1, 1)])), "at least 2"),
                  (lambda: info(show(cut(box(), [(0, 0, 0), (1, 0, 0)], mode="knife", keep="inside"), name="E")), "does not fit")):
    try:
        bad()
        ok(False, "must fail")
    except Exception as e:  # noqa: BLE001
        ok(want in str(e), str(e)[:80])
print("CUT SUITE OK")
