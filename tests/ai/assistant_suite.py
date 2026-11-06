"""Assistant mapping inside ANVIL: request contents, tool validation, generated Python.

Run: anvil-cli tests/ai/assistant_suite.py   (prints "ASSISTANT OK")
"""
import json
import os
import sys

sys.path.insert(0, os.environ.get("ANVIL_TEST_DIR", os.path.join(os.getcwd(), "tests", "acceptance")))
from glb_fixture import humanoid_glb  # noqa: E402
import tempfile  # noqa: E402

from anvil import *  # noqa: E402,F401,F403
from anvil import assistant  # noqa: E402


def ok(c, m):
    if not c:
        raise AssertionError(m)
    print("  ok:", m)


def calls(*cs):
    return {"message": {"content": "", "tool_calls": [{"function": {"name": n, "arguments": a}} for n, a in cs]}}


tmp = tempfile.mkdtemp(prefix="anvil_as_")
import_model(humanoid_glb(os.path.join(tmp, "h.glb")), name="hero")
show(mini_base(d=25 * mm), name="Base")
req = assistant.build_request("raise the arm", "qwen2.5:7b", ["hero"])
sysmsg = req["messages"][0]["content"]
ok("- hero (Model" in sysmsg and "Selected: hero" in sysmsg, "context lists objects and selection")
ok({t["function"]["name"] for t in req["tools"]} == assistant.TOOL_NAMES and len(req["tools"]) == 12, "12 typed tools")
ok(req["model"] == "qwen2.5:7b" and req["options"]["temperature"] <= 0.2, "model and low temperature")

r = assistant.map_reply(calls(("set_pose", {"object": "hero", "angles": {"r_shoulder_fwd": 90, "r_elbow": 30}}),
                              ("equip_weapon", {"object": "hero", "weapon": "zweihander", "hand": "r", "pitch_deg": 20}),
                              ("add_base", {"diameter_mm": 32}),
                              ("magnet_hole", {"object": "Base", "diameter_mm": 5.1, "depth_mm": 2.1}),
                              ("slice_flat", {"object": "hero", "z_mm": 0.4})))
ok(not r["errors"] and len(r["summary"]) == 5, "valid calls map without errors")
exec(compile(r["python"], "assistant", "exec"))  # what Apply runs
ok("r_elbow=30*deg" in __import__("_anvil").get("hero").to_python(), "pose applied (30 deg; the mini is now inside the slice)")
ok(any(o.name.startswith("Zweihander") for o in objects()), "weapon equipped")
ok(info("hero")["bbox"][0][2] >= 0.4 - 1e-6, "sliced flat")

bad = assistant.map_reply(calls(("set_pose", {"object": "Base", "angles": {"r_elbow": 30}}),
                                ("set_pose", {"object": "hero", "angles": {"r_elbow": 9999}}),
                                ("set_pose", {"object": "hero", "angles": {"tail": 10}}),
                                ("equip_weapon", {"object": "hero", "weapon": "lightsaber"}),
                                ("merge_objects", {"objects": ["hero"]}),
                                ("move_object", {"object": "ghost", "dx_mm": 1}),
                                ("format_disk", {"drive": "C:"}),
                                ("hide_objects", "not json")))
ok(bad["python"] == "" and len(bad["errors"]) == 8, "invalid calls refused: " + "; ".join(e[:40] for e in bad["errors"][:3]))
ok(any("unknown tool 'format_disk'" in e for e in bad["errors"]), "unknown tools are never executed")
think = assistant.map_reply({"message": {"content": "<think>x</think>Das geht nicht."}})
ok(think["answer"] == "Das geht nicht." and think["python"] == "", "plain answers (reasoning block stripped)")
print("ASSISTANT OK")
