"""Prints 'MAJOR.MINOR|has_libs|has_headers' for the running interpreter
(used by setup-windows.ps1 to find a Python that can build ANVIL)."""
import os
import sys
import sysconfig

base = sys.base_prefix
libs = os.path.isdir(os.path.join(base, "libs"))
headers = os.path.isfile(os.path.join(sysconfig.get_paths()["include"], "Python.h"))
print(f"{sys.version_info.major}.{sys.version_info.minor}|{libs}|{headers}")
