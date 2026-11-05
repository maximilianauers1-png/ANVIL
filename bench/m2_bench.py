"""M2 benchmark: B-Rep worker round trips, sketch solver, solidify, mixed booleans.

    ANVIL_BENCH_OUT=out.json anvil-cli bench/m2_bench.py

Every timing is the median of N runs of a fresh evaluation (caches defeated by
changing a parameter), wall clock in the embedded interpreter, i.e. including
the IPC to anvil-geom where the feature is exact.
"""
import json
import statistics
import sys
import time

from anvil import *  # noqa: F401,F403
import _anvil as _a

N = 5
results = {}


def bench(name, fn, n=N):
    ts = []
    for i in range(n):
        t0 = time.perf_counter()
        fn(i)
        ts.append((time.perf_counter() - t0) * 1e3)
    results[name] = {"median_ms": statistics.median(ts), "min_ms": min(ts), "max_ms": max(ts), "n": n}
    print(f"{name:55s} median {statistics.median(ts):9.2f} ms   (min {min(ts):.2f}, max {max(ts):.2f})")


# --- worker: start + trivial request
t0 = time.perf_counter()
s0 = Sketch("XY"); s0.rect((0, 0), (1, 1))
info(extrude(s0, 1 * mm))
results["first_exact_eval_incl_worker_start_ms"] = (time.perf_counter() - t0) * 1e3
print(f"{'first exact feature (starts anvil-geom)':55s} {results['first_exact_eval_incl_worker_start_ms']:9.2f} ms")

# --- exact features (each run a new parameter value -> no cache hit)
def extrude_rect(i):
    s = Sketch("XY")
    s.rect((0, 0), (40 + i * 0.1, 20))
    info(extrude(s, 5 * mm))
bench("extrude rectangle (B-Rep, IPC, tessellation)", extrude_rect)


def fillet_box(i):
    info(fillet(box(x=30 * mm, y=20 * mm, z=10 * mm + i * 0.01 * mm), 2 * mm, edges="all"))
bench("fillet all 12 edges of a box r=2 mm", fillet_box)


def shell_box(i):
    info(shell(box(x=30 * mm, y=20 * mm, z=10 * mm + i * 0.01 * mm), 1 * mm, faces="top"))
bench("shell box 1 mm, open top", shell_box)


# --- sketch solver: closed polyline with n corners, every segment H/V + lengths
def solver(n):
    # closed staircase of n segments (alternating H, V, length 2) plus 3 closing segments
    pts, x, y = [(0.0, 0.0)], 0.0, 0.0
    for k in range(n):
        if k % 2 == 0:
            x += 2.0
        else:
            y = 2.0 - y
        pts.append((x, y))
    def run(i):
        s = Sketch("XY")
        lines = s.polyline([(px * (1 + i * 1e-3), py) for px, py in pts] + [(pts[-1][0], -5), (0, -5)], closed=True)
        for L in lines[: n // 2]:
            s.dim("length", L, 2 * mm)
        s.fix(s.start(lines[0]))
        st = s.solve()
        assert st["converged"], st
    return run


for n in (10, 40, 100):
    bench(f"sketch solve: {n + 3}-gon, H/V + {n // 2} lengths + fix", solver(n))


# --- solidify a long trace (mask): 400 segments
def trace(i):
    pts = [(k * 0.5, 3 * ((k // 10) % 2) + i * 1e-3) for k in range(401)]
    s = Sketch("XY")
    s.polyline(pts)
    info(solidify(s, 0.3 * mm, 0.2 * mm))
bench("solidify 400-segment trace (Clipper2 + extrude)", trace)


# --- mixed: filleted plate (exact) minus solidified trace (mesh)
def mixed(i):
    plate = fillet(box(x=60 * mm, y=40 * mm, z=3 * mm + i * 0.01 * mm), 1 * mm, edges="vertical")
    s = Sketch("XY", offset=3 * mm)
    s.polyline([(-25, -15), (0, -15), (0, 15), (25, 15)])
    info(plate - solidify(s, 1 * mm, 1 * mm).translate(z=-0.5 * mm))
bench("mixed boolean: fillet plate (B-Rep) - solidify (mesh)", mixed)

import os
out = os.environ.get("ANVIL_BENCH_OUT")
if out:
    with open(out, "w") as f:
        json.dump(results, f, indent=1)
print("M2 BENCH DONE")
