"""Plugin loader (ADR 0014).

A plugin is a folder in the user plugin directory containing

    plugin.json   {"name": "...", "version": "1.0.0", "api": [1, 1]}
    plugin.py     def register(registry): ...   (anvil.commands.Registry)

`api` is the inclusive range of `anvil.__api_version__` the plugin supports;
incompatible plugins are skipped with a precise message instead of breaking
the application after an upgrade.
"""
from __future__ import annotations

import importlib.util
import json
import os

from . import __api_version__
from .commands import registry


def load_all(directory: str) -> list:
    report = []
    if not directory or not os.path.isdir(directory):
        return report
    for entry in sorted(os.listdir(directory)):
        folder = os.path.join(directory, entry)
        meta_path = os.path.join(folder, "plugin.json")
        if not os.path.isfile(meta_path):
            continue
        item = {"folder": entry, "loaded": False}
        try:
            with open(meta_path, encoding="utf-8") as f:
                meta = json.load(f)
            item.update(name=meta.get("name", entry), version=meta.get("version", "?"))
            lo, hi = meta.get("api", [1, 1])
            if not lo <= __api_version__ <= hi:
                item["error"] = f"needs API {lo}..{hi}, this ANVIL provides {__api_version__}"
            else:
                spec = importlib.util.spec_from_file_location(f"anvil_plugin_{entry}", os.path.join(folder, "plugin.py"))
                mod = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(mod)
                mod.register(registry)
                item["loaded"] = True
        except Exception as e:  # noqa: BLE001 -- a broken plugin must not break the app
            item["error"] = f"{type(e).__name__}: {e}"
        report.append(item)
    return report
