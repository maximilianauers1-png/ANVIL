# Runs inside anvil-cli (one command). Exercises the public API with asserts.
from anvil import *
import _anvil
import json
import math

# units
assert 25.4 * mm == 1 * inch
assert (12.5 * cm + 3 * mm).to(mm) == 128.0
try:
    25 * mm + 3 * deg
    raise AssertionError("expected TypeError")
except TypeError as e:
    assert str(e) == "cannot add Angle to Length", e

# plain numbers are rejected with a hint
try:
    cylinder(d=25, h=3 * mm)
    raise AssertionError("expected TypeError")
except TypeError as e:
    assert "write d=25*mm" in str(e), e

# solids and serialization round-trip
s = cylinder(d=25 * mm, h=3 * mm).chamfer(top=0.6 * mm) - cylinder(d=5.1 * mm, h=2.2 * mm)
assert repr(s) == "(cylinder(d=25*mm, h=3*mm, chamfer_top=0.6*mm) - cylinder(d=5.1*mm, h=2.2*mm))", repr(s)
t = box(x=10 * mm, y=20 * mm, z=5 * mm).rotate(z=30 * deg).translate(x=1 * mm)
assert "rz=30*deg" in repr(t), repr(t)
assert eval(repr(t)).__repr__() == repr(t)

# parametric variables
var("r", 12.5 * mm)
o = show(cylinder(d="2*r", h=3 * mm), name="P")
i1 = info(o)
var("r", 10 * mm)
i2 = info(o)
assert i2["volume"] < i1["volume"]
assert abs(i2["bbox"][1][0] - 10.0) < 1e-9

# checks
rep = check(o)
assert rep["ok"] and rep["shells"][0]["genus"] == 0, rep
torus_rep = check(torus(R=20 * mm, r=5 * mm), chord=0.05 * mm)
assert torus_rep["shells"][0]["genus"] == 1

# move folds translations; edit by path
show(box(), name="B")
move("B", x=5 * mm)
move("B", x=5 * mm, z=1 * mm)
p = _anvil.get("B").params()
assert p["tx"][0] == 10.0 and p["tz"][0] == 1.0
edit("B", "src", x=4 * mm)
assert _anvil.get("B").child("src").params()["x"][0] == 4.0

# booleans on objects consume the tools
show(box(x=30 * mm, y=30 * mm, z=5 * mm), name="Plate")
show(cylinder(d=6 * mm, h=5 * mm), name="Hole")
subtract("Plate", "Hole")
names = [x.name for x in objects()]
assert "Hole" not in names and "Plate" in names

# command registry: every UI action becomes Python
from anvil.commands import registry
src = registry.build("create.cylinder", {"selection": [], "point": [1.5, 0, 0]})
assert src == "show(cylinder(d=20*mm, h=10*mm).translate(x=1.5*mm))", src
assert registry.build("boolean.subtract", {"selection": ["A", "B"]}) == "subtract('A', 'B')"
data = json.loads(registry.to_json())
assert len(data["radial"]["Design"]["empty"]) == 8
assert all(len(ring) <= 8 for mode in data["radial"].values() for ring in mode.values())

print("API SUITE OK")
