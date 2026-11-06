"""Runs the real application in self-test mode (synthetic mouse/keyboard input
through the M1 acceptance flow) and compares the exported STL with the one the
acceptance script produces. Exit code 77 = skipped (no display available).

usage: run_gui_selftest.py <anvil> <anvil-cli> <magnet_base.py>
"""
import hashlib
import json
import os
import subprocess
import sys
import tempfile

app, cli, script = sys.argv[1:4]
if sys.platform.startswith("linux") and not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
    print("no display: skipped (run under Xvfb, see docs/ENTWICKLUNG.md)")
    sys.exit(77)

with tempfile.TemporaryDirectory() as d:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from glb_fixture import humanoid_glb
    glb = humanoid_glb(os.path.join(d, "hero.glb"))  # M4 steps: a rigged mini as Tripo / Meshy export it
    env = dict(os.environ, XDG_CONFIG_HOME=os.path.join(d, "cfg"), XDG_DATA_HOME=os.path.join(d, "data"),
               APPDATA=os.path.join(d, "cfg"), LOCALAPPDATA=os.path.join(d, "data"), ANVIL_TEST_GLB=glb)
    p = subprocess.run([app, "--selftest", os.path.join(d, "st")], env=env, capture_output=True, text=True, timeout=900)
    print(p.stdout[-4000:])
    report = json.load(open(os.path.join(d, "st", "report.json")))
    assert report["ok"], "self-test failed: " + json.dumps([s for s in report["steps"] if not s.get("ok")])
    assert report["radial_frames_to_visible"] <= 1, report["radial_frames_to_visible"]
    subprocess.run([cli, script], cwd=d, check=True, capture_output=True)
    h = [hashlib.sha256(open(os.path.join(d, f), "rb").read()).hexdigest() for f in ("st/ui.stl", "base_25mm.stl")]
    print("ui.stl     ", h[0])
    print("script stl ", h[1])
    assert h[0] == h[1], "UI and script geometry differ"
    print("GUI acceptance OK: identical STL, radial menu visible after", report["radial_frames_to_visible"], "frame(s)")
