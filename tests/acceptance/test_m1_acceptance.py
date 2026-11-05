"""M1 acceptance tests, driven through anvil-cli (the same command bus as the app).

1. Magnet base via script and via the exact command sequence the UI generates
   -> bit-identical STL, and the mesh passes every check.
2. Crash recovery: journal written by one process, replayed by another ->
   identical document; an unfinished command is reported, not replayed.
3. Every Python command is one undo step; failing commands change nothing.

Run: python tests/acceptance/test_m1_acceptance.py <path-to-anvil-cli>
"""
import hashlib
import os
import subprocess
import sys
import tempfile
import unittest

CLI = None
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def run(*args, cwd=None):
    p = subprocess.run([CLI, *args], cwd=cwd, capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


# Exactly what the UI emits (commands.py builders + property edits + export dialog).
UI_SEQUENCE = [
    "show(cylinder(d=20*mm, h=10*mm))",
    'edit("Cylinder1", d=25*mm)',
    'edit("Cylinder1", h=3*mm)',
    'edit("Cylinder1", chamfer_top=0.6*mm)',
    "show(cylinder(d=20*mm, h=10*mm))",
    'edit("Cylinder2", d=5.1*mm)',
    'edit("Cylinder2", h=2.2*mm)',
    "subtract('Cylinder1', 'Cylinder2')",
    'export_stl("Cylinder1", "ui.stl", unit=mm, chord=0.01*mm)',
]


class MagnetBase(unittest.TestCase):
    def test_script_and_ui_identical(self):
        with tempfile.TemporaryDirectory() as d:
            rc, out, err = run(os.path.join(ROOT, "examples", "magnet_base.py"), cwd=d)
            self.assertEqual(rc, 0, err)
            args = []
            for c in UI_SEQUENCE:
                args += ["-c", c]
            rc, out, err = run(*args, cwd=d)
            self.assertEqual(rc, 0, err)
            a, b = os.path.join(d, "base_25mm.stl"), os.path.join(d, "ui.stl")
            self.assertEqual(sha(a), sha(b), "UI and script geometry differ")
            n = (os.path.getsize(a) - 84) // 50
            self.assertEqual(os.path.getsize(a), 84 + 50 * n)
            with open(a, "rb") as f:
                self.assertIn(b"unit=mm", f.read(80))

    def test_not_printable_is_refused_with_reason(self):
        with tempfile.TemporaryDirectory() as d:
            stl = os.path.join(d, "open.stl")
            # an open mesh: a single triangle
            import struct
            with open(stl, "wb") as f:
                f.write(b"x" * 80 + struct.pack("<I", 1) + struct.pack("<12fH", 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0))
            rc, out, err = run("-c", f"import_stl(r'{stl}')", "-c", f"export_stl('open', r'{d}/x.stl')", cwd=d)
            self.assertNotEqual(rc, 0)
            self.assertIn("not printable, nothing was written", err)
            self.assertIn("open (boundary) edge", err)
            self.assertFalse(os.path.exists(os.path.join(d, "x.stl")))


class Recovery(unittest.TestCase):
    def test_replay_restores_document(self):
        with tempfile.TemporaryDirectory() as d:
            j = os.path.join(d, "journal.py")
            args = ["--journal", j, "--print-doc"]
            for c in UI_SEQUENCE[:-1]:
                args += ["-c", c]
            args += ["-c", "edit('Cylinder1', 'a', d=30*mm)", "-c", "this_fails()"]
            rc, doc1, err = run(*args, cwd=d)
            self.assertEqual(rc, 1)  # the failing command
            # simulate a crash during one more command: append a txn without ok marker
            with open(j, "ab") as f:  # binary: the journal format uses LF on every platform
                src = "show(sphere(d=1*mm))"
                f.write(f"#@txn 99 ui crc={crc32c(src.encode()):08x} bytes={len(src)}\n{src}\n#@end 99\n".encode())
            rc, doc2, err = run("--replay", j, "--print-doc", cwd=d)
            self.assertEqual(rc, 0, err)
            self.assertIn("unfinished (possible crash cause): show(sphere(d=1*mm))", doc2)
            self.assertIn("skipped 1 failed", doc2)
            self.assertEqual(doc1.strip().splitlines()[-1], doc2.strip().splitlines()[-1])


def crc32c(data: bytes) -> int:
    c = 0xFFFFFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
    return c ^ 0xFFFFFFFF


class ApiInsideSession(unittest.TestCase):
    def test_api_suite(self):
        rc, out, err = run(os.path.join(HERE, "api_suite.py"))
        self.assertEqual(rc, 0, out + err)
        self.assertIn("API SUITE OK", out)


if __name__ == "__main__":
    CLI = os.path.abspath(sys.argv.pop(1))
    unittest.main()
