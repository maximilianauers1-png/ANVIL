"""M4 acceptance: imported minis (Tripo/Meshy-style GLB, unrigged models),
posing, weapons in hand, merge with a base into one printable object.

Run: anvil-cli tests/acceptance/m4_suite.py   (prints "M4 SUITE OK")
"""
import os
import sys
import tempfile
import time

# anvil-cli runs the script as a command (no __file__): ctest passes the directory
_here = os.environ.get("ANVIL_TEST_DIR", os.path.join(os.getcwd(), "tests", "acceptance"))
sys.path.insert(0, _here)
from glb_fixture import humanoid_glb  # noqa: E402

from anvil import *  # noqa: E402,F401,F403

tmp = tempfile.mkdtemp(prefix="anvil_m4_")
rigged = humanoid_glb(os.path.join(tmp, "hero.glb"), rigged=True)
plain = humanoid_glb(os.path.join(tmp, "statue.glb"), rigged=False)


def get_attrs(name):
    return dict(__import__("_anvil").get(name).attrs())


def ok(cond, msg):
    if not cond:
        raise AssertionError(msg)
    print("  ok:", msg)


print("1. import: rig from the file / automatic rig, scaled to 32 mm, standing on z = 0")
h = import_model(rigged, height=32 * mm, name="Hero")
mi = model_info(h)
ok(mi["rig"] == "file" and "mixamorig:LeftForeArm" in mi["joints"], "rigged GLB keeps its Mixamo skeleton")
bb = info(h)["bbox"]
ok(abs(bb[1][2] - 32.0) < 1e-6 and abs(bb[0][2]) < 1e-9, "height 32 mm, feet on z = 0")
ok(bb[1][0] - bb[0][0] < 20, "T-pose arms normalised to hanging (width %.1f mm)" % (bb[1][0] - bb[0][0]))
s = import_model(plain, name="Statue")
ok(model_info(s)["rig"] == "automatic" and len(model_info(s)["joints"]) == 17, "unrigged model gets a 17-joint rig")

print("2. posing: presets and single angles, one undo step each")
before = info(h)["bbox"]
pose(h, "attack")
after = info(h)["bbox"]
ok(after != before, "attack preset moves the model")
edit(h, r_elbow=80 * deg)
ok(__import__("_anvil").get("Hero").params()["r_elbow"][0] != 0, "angles stay editable")

print("3. weapons: every type, in either hand, linked to the pose")
for i, w in enumerate(WEAPONS):
    o = equip(h, w, hand="r" if i % 2 == 0 else "l", name="W_" + w)
    c = check(o)
    ok(c["ok"], f"{w}: closed and printable ({c['summary']})")
z0 = info("W_longsword")["bbox"]
pose(h, "guard")
ok(info("W_longsword")["bbox"] != z0, "the sword follows a pose change")
for w in WEAPONS[1:]:
    delete("W_" + w)

print("4. merge: mini + sword + base -> one closed, manifold, void-free object")
b = show(mini_base(d=25 * mm), name="Base")
t = time.time()
m = merge("Hero", "W_longsword", "Base", name="Mini")
c = check(m)
ok(c["ok"] and len(c["shells"]) == 1, f"merged: {c['summary']} in {time.time() - t:.1f} s")
pose("Statue", "walk")
c2 = check(merge("Statue", equip("Statue", "buster"), name="Mini2"))
ok(c2["ok"], f"automatic rig + buster sword: {c2['summary']}")
stl = os.path.join(tmp, "mini.stl")
r = export_stl("Mini", stl)
ok(os.path.getsize(stl) > 1000, "STL exported (%d triangles)" % r["triangles"])

print("5. save / load reproduces the document")
doc = os.path.join(tmp, "m4.anvil")
save(doc)
src = open(doc, encoding="utf-8").read()
ok("model_file(" in src and "attach(weapon(" in src and "sha256=" in src, "document stores file, hash, pose, weapons")
vol = info("Mini")["volume"]
load(doc)
ok(abs(info("Mini")["volume"] - vol) < 1e-9, "reload gives the same merged volume")

print("6. distortion fixes: rigid armour plates, geodesic weights, file rest pose")
pd = humanoid_glb(os.path.join(tmp, "pauldrons.glb"), rigged=True, pauldrons=True)
a = import_model(pd, name="Armour")
ok(model_info(a)["rigid_parts"] >= 2, "separate shoulder plates bound rigidly (%d rigid shells)" % model_info(a)["rigid_parts"])
pose(a, "attack")
d = pose_distortion(a)
ok(d["p99"] < 3.0, "pose distortion measured: p99 edge ratio %.2f" % d["p99"])
model_options(a, weights="auto")
ok(get_attrs("Armour").get("weights") == "auto", "geodesic weights for the file's skeleton")
model_options(a, rest="file")
w_file = info(a)["bbox"][1][0] - info(a)["bbox"][0][0]
pose(a, "stand")
ok(w_file > 0, "rest='file' evaluates")
w = info(a)["bbox"][1][0] - info(a)["bbox"][0][0]
ok(w > 20, "rest='file': pose 0 keeps the T-pose of the file (width %.1f mm)" % w)
delete("Armour")

print("7. precise errors")
try:
    attach(weapon("dagger"), "Base")
    show(attach(weapon("dagger"), "Base"), name="Bad")
    info("Bad")
    ok(False, "attach to a base must fail")
except Exception as e:  # noqa: BLE001
    ok("not an imported model" in str(e) or "rig" in str(e), "attach to a non-model: " + str(e)[:80])
if "Bad" in [o.name for o in objects()]:
    delete("Bad")
bad = os.path.join(tmp, "x.fbx")
open(bad, "w").write("x")
try:
    import_model(bad)
    ok(False, "fbx must be refused")
except Exception as e:  # noqa: BLE001
    ok(".glb" in str(e), "unsupported format named: " + str(e)[:80])

print("M4 SUITE OK")
