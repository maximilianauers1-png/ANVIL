# Primitives added after M3: analytic volumes, export checks, precise errors.
import math
from anvil import *
pi = math.pi
def close(a, b, rel, what):
    assert abs(a - b) <= rel * abs(b), f"{what}: {a} vs {b}"
def V(s):
    i = info(s); assert i["manifold"], i; assert check(s)["ok"], check(s)["summary"]; return i["volume"]
close(V(tube(d=20*mm, d_in=16*mm, h=10*mm)), pi/4*(400-256)*10, 2e-3, "tube")
close(V(prism(n=6, d=20*mm, h=10*mm)), 3*math.sqrt(3)/2*100*10, 1e-9, "hex prism")
close(V(pyramid(n=4, d=20*mm, h=15*mm)), (2*100)*15/3, 1e-9, "pyramid")    # square of diagonal 20: area 200
close(V(capsule(d=10*mm, h=30*mm)), pi*25*20 + 4/3*pi*125, 3e-3, "capsule")
r = 2
close(V(rounded_box(x=20*mm, y=20*mm, z=10*mm, r=2*mm)), 16*16*6 + 2*r*(16*16+16*6+16*6) + pi*r*r*(16+16+6) + 4/3*pi*r**3, 5e-3, "rounded box (Steiner)")
close(V(wedge(x=20*mm, y=10*mm, z=10*mm)), 0.5*20*10*10, 1e-9, "wedge")
close(V(ellipsoid(x=30*mm, y=20*mm, z=10*mm)), 4/3*pi*15*10*5, 3e-3, "ellipsoid")
n, R1, R0 = 5, 15, 7
close(V(star(n=5, d=30*mm, d_in=14*mm, h=4*mm)), 2*n*0.5*R1*R0*math.sin(pi/n)*4, 1e-9, "star")
for bad, msg in ((lambda: tube(d=10*mm, d_in=12*mm), "d_in"), (lambda: prism(n=2.5), "whole number"),
                 (lambda: capsule(d=10*mm, h=5*mm), "at least d"), (lambda: rounded_box(r=6*mm, z=10*mm), "half")):
    try:
        info(bad()); raise AssertionError("no error: " + msg)
    except (ValueError, GeometryError) as e:
        assert msg in str(e), e
p = rounded_box(x=20*mm, y=20*mm, z=10*mm, r=2*mm) - tube(d=8*mm, d_in=4*mm, h=10*mm)
assert check(p)["ok"]
print(prism(n=6, d=20*mm, h=10*mm))
print("SHAPES OK")
