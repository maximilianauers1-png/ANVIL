"""Analytische Validierung der Referenzimplementierung.

Ausführen:  python -m unittest discover -s tests/reference -v
"""
from __future__ import annotations

import math
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))

import meshcheck  # noqa: E402
import primitives as P  # noqa: E402
import stl  # noqa: E402
from tolerances import close  # noqa: E402

MM_PER_INCH = 25.4  # exakt per Definition (1959)


class Units(unittest.TestCase):
    def test_inch_roundtrip(self):
        self.assertTrue(close(1.0 * MM_PER_INCH, 25.4))
        self.assertTrue(close(25.4 / MM_PER_INCH, 1.0))
        # Roundtrip über viele Werte: höchstens wenige ulp Abweichung.
        rng = np.random.default_rng(1)
        for x in rng.uniform(-1e4, 1e4, 10_000):
            self.assertTrue(close((x / MM_PER_INCH) * MM_PER_INCH, x), x)


class Chord(unittest.TestCase):
    def test_formula(self):
        # Beispiel aus der Ziel-API: d = 25 mm, δ = 0.01 mm
        n = P.segments_for_chord(12.5, 0.01)
        self.assertEqual(n, math.ceil(math.pi / math.acos(1 - 0.01 / 12.5)))
        self.assertEqual(n, 79)
        # Sehnenfehler eingehalten, mit n−1 verletzt
        sag = lambda k: 12.5 * (1 - math.cos(math.pi / k))  # noqa: E731
        self.assertLessEqual(sag(n), 0.01)
        self.assertGreater(sag(n - 1), 0.01)


class Primitives(unittest.TestCase):
    def assertValid(self, V, F, genus=0, shells=1):
        rep = meshcheck.check(V, F)
        self.assertTrue(rep.ok, rep.errors)
        self.assertEqual(len(rep.shells), shells)
        for s in rep.shells:
            self.assertEqual(s.genus, genus)
            self.assertEqual(3 * s.faces, 2 * s.edges)
        return rep

    def test_cube_exact(self):
        rep = self.assertValid(*P.cube(2.0))
        self.assertTrue(close(rep.volume, 8.0))
        self.assertTrue(close(rep.area, 24.0))

    def test_sphere_convergence_order_2(self):
        """Einbeschriebene Polyeder: Volumen- und Flächenfehler = O(h²).
        Jede Unterteilung halbiert h ⇒ Fehlerquotient → 4 (Ordnung p = log2 q → 2)."""
        R = 10.0
        errs_v, errs_a = [], []
        for lvl in range(2, 7):
            rep = self.assertValid(*P.icosphere(lvl, R))
            errs_v.append(abs(rep.volume - 4 / 3 * math.pi * R**3))
            errs_a.append(abs(rep.area - 4 * math.pi * R**2))
        for e in (errs_v, errs_a):
            p = math.log2(e[-2] / e[-1])
            self.assertAlmostEqual(p, 2.0, delta=0.05)

    def test_torus_convergence_order_2(self):
        R, r = 20.0, 5.0
        V_ex, A_ex = 2 * math.pi**2 * R * r**2, 4 * math.pi**2 * R * r
        prev = None
        for k in (32, 64, 128, 256):
            rep = self.assertValid(*P.torus(R, r, 2 * k, k), genus=1)
            e = (abs(rep.volume - V_ex), abs(rep.area - A_ex))
            if prev:
                for a, b in zip(prev, e):
                    self.assertAlmostEqual(math.log2(a / b), 2.0, delta=0.05)
            prev = e

    def test_cylinder(self):
        n = P.segments_for_chord(12.5, 0.01)
        rep = self.assertValid(*P.cylinder(12.5, 3.0, n))
        # Exaktes Volumen des n-Ecks: (n/2)·r²·sin(2π/n)·h
        self.assertTrue(close(rep.volume, n / 2 * 12.5**2 * math.sin(2 * math.pi / n) * 3.0, rel=1e-12))

    def test_two_shells(self):
        V1, F1 = P.cube(1.0)
        V2, F2 = P.torus(5, 1, 24, 12)
        rep = meshcheck.check(np.vstack([V1, V2 + 10]), np.vstack([F1, F2 + len(V1)]))
        self.assertTrue(rep.ok, rep.errors)
        self.assertEqual(sorted(s.genus for s in rep.shells), [0, 1])


class Defects(unittest.TestCase):
    """Jeder Defekt muss präzise benannt werden."""

    def test_open_mesh(self):
        V, F = P.cube()
        rep = meshcheck.check(V, F[:-1])
        self.assertFalse(rep.ok)
        self.assertTrue(any("boundary" in e for e in rep.errors), rep.errors)

    def test_flipped_face(self):
        V, F = P.cube()
        F = F.copy()
        F[0] = F[0, ::-1]
        rep = meshcheck.check(V, F)
        self.assertFalse(rep.ok)
        self.assertTrue(any("half-edge" in e for e in rep.errors), rep.errors)

    def test_inverted_shell(self):
        V, F = P.icosphere(2)
        rep = meshcheck.check(V, F[:, ::-1])
        self.assertFalse(rep.ok)
        self.assertTrue(any("signed volume" in e for e in rep.errors), rep.errors)

    def test_degenerate(self):
        V, F = P.cube()
        V = np.vstack([V, V[0]])  # Duplikat von Vertex 0 → Nullfläche
        F = np.vstack([F, [[0, 1, 8]], [[0, 8, 1]]])
        rep = meshcheck.check(V, F)
        self.assertFalse(rep.ok)
        self.assertTrue(any("degenerate" in e for e in rep.errors), rep.errors)

    def test_bowtie_vertex(self):
        """Zwei Tetraeder, die sich nur einen Vertex teilen: kantenmannigfaltig,
        aber nicht vertexmannigfaltig."""
        T = np.array([[0, 2, 1], [0, 1, 3], [1, 2, 3], [0, 3, 2]])
        V = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1],
                      [-1, 0, 0], [0, -1, 0], [0, 0, -1]], float)
        T2 = np.array([[0, 5, 4], [0, 4, 6], [4, 5, 6], [0, 6, 5]])
        rep = meshcheck.check(V, np.vstack([T, T2]))
        self.assertFalse(rep.ok)
        self.assertTrue(any("non-manifold vertex #0" in e for e in rep.errors), rep.errors)


class StlFormat(unittest.TestCase):
    def test_size_and_roundtrip(self):
        V, F = P.icosphere(3, 12.5)
        rec = stl.to_records(V, F)
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "s.stl"
            stl.write_binary(p, rec)
            self.assertEqual(p.stat().st_size, 84 + 50 * len(F))
            hdr, back = stl.read_binary(p)
            self.assertIn(b"unit=mm", hdr)
            np.testing.assert_array_equal(back["v"], V[F].astype(np.float32))


if __name__ == "__main__":
    unittest.main()
