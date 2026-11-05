"""Builds the stdlib zip for the embedded interpreter from a local Python
installation (no download). Excludes tests, IDLE, tkinter, ensurepip,
site-packages and every networking module, so code running inside ANVIL
cannot open network connections (requirement 2.1).

usage: python make_python_zip.py <Lib dir> <out.zip>
"""
import compileall
import os
import sys
import zipfile

EXCLUDE_DIRS = {"test", "tests", "idlelib", "tkinter", "turtledemo", "ensurepip", "site-packages", "venv",
                "lib2to3", "__pycache__", "urllib", "http", "xmlrpc", "email", "asyncio"}
EXCLUDE_FILES = {"socket.py", "ssl.py", "socketserver.py", "ftplib.py", "smtplib.py", "poplib.py", "imaplib.py",
                 "nntplib.py", "telnetlib.py", "webbrowser.py", "turtle.py"}


def main(lib: str, out: str) -> int:
    n = 0
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for base, dirs, files in os.walk(lib):
            dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
            for f in files:
                if not f.endswith(".py") or f in EXCLUDE_FILES:
                    continue
                full = os.path.join(base, f)
                rel = os.path.relpath(full, lib)
                z.write(full, rel)
                n += 1
    print(f"{out}: {n} modules")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
