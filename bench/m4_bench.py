"""M4 benchmark: import, rig, pose, weapon, merge of AI-sized minis.

Run: anvil-cli bench/m4_bench.py   (ANVIL_TEST_DIR=tests/acceptance; writes bench/out/M4-<host>.json)
Mesh sizes like Tripo / Meshy exports: ~5k (low), ~100k and ~400k triangles.
"""
import json
import os
import platform
import sys
import tempfile
import time

sys.path.insert(0, os.environ.get("ANVIL_TEST_DIR", os.path.join(os.getcwd(), "tests", "acceptance")))
from glb_fixture import humanoid_glb  # noqa: E402

from anvil import *  # noqa: E402,F401,F403

tmp = tempfile.mkdtemp(prefix="anvil_m4b_")
rows = []
for label, sides, rings in (("5k", 8, 10), ("100k", 48, 64), ("400k", 96, 128)):
    for rigged in (True, False):
        clear()
        path = humanoid_glb(os.path.join(tmp, f"m_{label}_{rigged}.glb"), rigged=rigged, sides=sides, rings=rings)
        r = {"mesh": label, "rig": "file" if rigged else "automatic"}
        t = time.perf_counter(); m = import_model(path, name="M"); r["tris"] = info(m)["triangles"]
        r["import_s"] = time.perf_counter() - t
        t = time.perf_counter(); pose(m, "attack"); info(m); r["pose_s"] = time.perf_counter() - t
        t = time.perf_counter(); w = equip(m, "zweihander"); info(w); r["weapon_s"] = time.perf_counter() - t
        b = show(mini_base(d=25 * mm), name="B"); info(b)
        t = time.perf_counter(); mm_ = merge(m, w, b, name="Mini"); i = info(mm_); r["merge_s"] = time.perf_counter() - t
        t = time.perf_counter(); c = check(mm_); r["check_s"] = time.perf_counter() - t
        r["ok"] = c["ok"]; r["merged_tris"] = c["triangles"]
        rows.append(r)
        print(json.dumps(r))
os.makedirs("bench/out", exist_ok=True)
out = {"host": platform.node(), "cpu_count": os.cpu_count(), "platform": platform.platform(), "rows": rows}
json.dump(out, open(os.path.join("bench", "out", f"M4-{platform.node()}.json"), "w"), indent=1)
print("M4 BENCH DONE")
