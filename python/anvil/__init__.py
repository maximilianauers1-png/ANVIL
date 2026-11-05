"""ANVIL Python API.

    from anvil import *
    part = cylinder(d=25*mm, h=3*mm).chamfer(top=0.6*mm) - cylinder(d=5.1*mm, h=2.2*mm)
    show(part)
    export_stl(part, "base_25mm.stl", unit=mm, chord=0.01*mm)
"""
import _anvil

__api_version__ = _anvil.API_VERSION
__version__ = _anvil.version()

from .units import Quantity, cm, deg, inch, m, mm, rad, um  # noqa: E402
from .api import *  # noqa: E402,F401,F403
from .api import __all__ as _api_all  # noqa: E402

__all__ = ["Quantity", "mm", "cm", "m", "um", "inch", "deg", "rad"] + list(_api_all)
