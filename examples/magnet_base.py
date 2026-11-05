# M1 acceptance: 25 mm magnet base (5 x 2 mm magnet, +0.1 mm clearance).
from anvil import *

base = cylinder(d=25*mm, h=3*mm).chamfer(top=0.6*mm)
pocket = cylinder(d=5.1*mm, h=2.2*mm)
part = base - pocket
show(part)
export_stl(part, "base_25mm.stl", unit=mm, chord=0.01*mm)
