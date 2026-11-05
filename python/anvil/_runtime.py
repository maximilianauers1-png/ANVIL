"""Execution runtime used by the C++ command bus (internal).

run()      executes a command (one transaction, managed by C++)
check()    compiles only (live syntax errors in the Command Bar)
complete() autocompletion candidates
query()    read-only JSON queries for the UI (command registry, radial layout)
"""
from __future__ import annotations

import ast
import contextlib
import io
import json
import re
import rlcompleter
import sys
import traceback

_NS: dict = {}
_plugin_report: list = []


def init(plugin_dir: str = ""):
    global _NS
    _NS = {"__name__": "__anvil__", "__builtins__": __builtins__}
    exec("from anvil import *", _NS)
    if plugin_dir:
        from . import plugins
        _plugin_report.extend(plugins.load_all(plugin_dir))


def _error(e: BaseException, source: str) -> dict:
    line, col = 0, 0
    if isinstance(e, SyntaxError):
        line, col = e.lineno or 0, e.offset or 0
        msg = f"SyntaxError: {e.msg}"
    else:
        for fr in traceback.extract_tb(e.__traceback__):
            if fr.filename == "<command>":
                line = fr.lineno or 0
        msg = f"{type(e).__name__}: {e}"
    if line:
        msg = f"line {line}: {msg}"
    return {"ok": False, "error": msg, "line": line, "col": col}


def run(source: str, mode: str) -> dict:
    from . import api
    out = io.StringIO()
    api._state["shown"] = False
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            if mode == "preview":
                val = eval(compile(source, "<command>", "eval"), _NS)
                if isinstance(val, api.Obj):
                    val = val.solid
                if isinstance(val, api.Solid):
                    import _anvil
                    _anvil.set_preview(val._node)
                return {"ok": True, "output": out.getvalue()}
            tree = ast.parse(source, "<command>", "exec")
            repl = mode == "console"
            last = tree.body[-1] if repl and tree.body and isinstance(tree.body[-1], ast.Expr) else None
            if last is not None:
                tree.body = tree.body[:-1]
            exec(compile(tree, "<command>", "exec"), _NS)
            if last is not None:
                val = eval(compile(ast.Expression(last.value), "<command>", "eval"), _NS)
                # REPL convenience: a bare solid expression is shown
                if isinstance(val, api.Solid) and not api._state["shown"]:
                    api.show(val)
                elif val is not None and not isinstance(val, api.Obj):
                    print(repr(val))
        return {"ok": True, "output": out.getvalue()}
    except BaseException as e:  # noqa: BLE001 -- report everything, including SystemExit
        r = _error(e, source)
        r["output"] = out.getvalue()
        return r


def check(source: str) -> dict:
    try:
        compile(source, "<command>", "exec")
        return {"ok": True}
    except SyntaxError as e:
        return _error(e, source)


_TOKEN = re.compile(r"[A-Za-z_][A-Za-z0-9_\.]*$")


def complete(source: str) -> dict:
    m = _TOKEN.search(source)
    prefix = m.group(0) if m else ""
    comp = rlcompleter.Completer(_NS)
    out = []
    if prefix:
        for i in range(200):
            c = comp.complete(prefix, i)
            if c is None:
                break
            if not c.split(".")[-1].startswith("_"):
                out.append(c)
    return {"ok": True, "json": json.dumps({"prefix": prefix, "items": sorted(set(out))[:50]})}


def query(q: str) -> dict:
    from .commands import registry
    try:
        req = json.loads(q) if q.strip().startswith("{") else {"q": q.strip()}
        name = req.get("q")
        if name == "commands":
            return {"ok": True, "json": registry.to_json()}
        if name == "build":
            src = registry.build(req["id"], req.get("ctx", {}))
            return {"ok": True, "json": json.dumps({"source": src})}
        if name == "plugins":
            return {"ok": True, "json": json.dumps(_plugin_report)}
        if name == "signature":
            import inspect
            obj = eval(req["name"], _NS)
            doc = (inspect.getdoc(obj) or "").split("\n\n")[0]
            return {"ok": True, "json": json.dumps({"signature": f"{req['name']}{inspect.signature(obj)}", "doc": doc})}
        return {"ok": False, "error": f"unknown query '{name}'"}
    except Exception as e:  # noqa: BLE001
        return {"ok": False, "error": f"{type(e).__name__}: {e}"}
