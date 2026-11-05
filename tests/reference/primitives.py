"""Deterministische Referenz-Primitive als indizierte Dreiecksnetze (float64, mm).

Alle Netze sind geschlossen, 2-mannigfaltig und außen orientiert (CCW von außen),
so dass das signierte Volumen positiv ist. Sie dienen als Golden-Referenz für die
C++-Implementierung (M1) und als Bausteine der Benchmark-Szene.
"""
from __future__ import annotations

import math

import numpy as np


def segments_for_chord(radius_mm: float, chord_mm: float) -> int:
    """Segmentzahl n = ⌈π / arccos(1 − δ/r)⌉ für Sehnenfehler δ bei Radius r.

    Herleitung: Ein Segment mit halbem Öffnungswinkel θ hat die Pfeilhöhe
    s = r(1 − cos θ). Aus s ≤ δ folgt θ ≤ arccos(1 − δ/r); mit 2θ·n = 2π folgt
    n ≥ π/θ. Für δ ≥ 2r ist bereits n = 3 hinreichend (Dreieck).
    """
    if radius_mm <= 0 or chord_mm <= 0:
        raise ValueError("radius and chord must be positive")
    x = 1.0 - chord_mm / radius_mm
    if x <= -1.0:
        return 3
    return max(3, math.ceil(math.pi / math.acos(x)))


def cube(edge: float = 1.0) -> tuple[np.ndarray, np.ndarray]:
    h = edge / 2.0
    V = np.array([[x, y, z] for z in (-h, h) for y in (-h, h) for x in (-h, h)], dtype=np.float64)
    # Vertex-Index: i = x + 2y + 4z mit x,y,z ∈ {0,1}
    F = np.array([
        [0, 2, 3], [0, 3, 1],  # z = -h, Normale -z
        [4, 5, 7], [4, 7, 6],  # z = +h
        [0, 1, 5], [0, 5, 4],  # y = -h
        [2, 6, 7], [2, 7, 3],  # y = +h
        [0, 4, 6], [0, 6, 2],  # x = -h
        [1, 3, 7], [1, 7, 5],  # x = +h
    ], dtype=np.int64)
    return V, F


def icosphere(level: int, radius: float = 1.0) -> tuple[np.ndarray, np.ndarray]:
    """Ikosaeder, `level`-mal 1:4 unterteilt und auf die Kugel projiziert. F = 20·4^level."""
    t = (1.0 + math.sqrt(5.0)) / 2.0
    V = np.array([
        [-1, t, 0], [1, t, 0], [-1, -t, 0], [1, -t, 0],
        [0, -1, t], [0, 1, t], [0, -1, -t], [0, 1, -t],
        [t, 0, -1], [t, 0, 1], [-t, 0, -1], [-t, 0, 1],
    ], dtype=np.float64)
    F = np.array([
        [0, 11, 5], [0, 5, 1], [0, 1, 7], [0, 7, 10], [0, 10, 11],
        [1, 5, 9], [5, 11, 4], [11, 10, 2], [10, 7, 6], [7, 1, 8],
        [3, 9, 4], [3, 4, 2], [3, 2, 6], [3, 6, 8], [3, 8, 9],
        [4, 9, 5], [2, 4, 11], [6, 2, 10], [8, 6, 7], [9, 8, 1],
    ], dtype=np.int64)
    V /= np.linalg.norm(V, axis=1, keepdims=True)
    for _ in range(level):
        e = np.concatenate([F[:, [0, 1]], F[:, [1, 2]], F[:, [2, 0]]])
        e.sort(axis=1)
        uniq, inv = np.unique(e, axis=0, return_inverse=True)
        inv = inv.reshape(-1)
        mids = V[uniq[:, 0]] + V[uniq[:, 1]]
        mids /= np.linalg.norm(mids, axis=1, keepdims=True)
        m = len(F)
        a, b, c = F[:, 0], F[:, 1], F[:, 2]
        ab = inv[:m] + len(V)
        bc = inv[m:2 * m] + len(V)
        ca = inv[2 * m:] + len(V)
        V = np.concatenate([V, mids])
        F = np.concatenate([
            np.stack([a, ab, ca], 1), np.stack([b, bc, ab], 1),
            np.stack([c, ca, bc], 1), np.stack([ab, bc, ca], 1),
        ])
    return V * radius, F


def torus(R: float, r: float, n_major: int, n_minor: int) -> tuple[np.ndarray, np.ndarray]:
    """Torus um die z-Achse; F = 2·n_major·n_minor."""
    u = np.arange(n_major) * (2.0 * math.pi / n_major)
    v = np.arange(n_minor) * (2.0 * math.pi / n_minor)
    uu, vv = np.meshgrid(u, v, indexing="ij")
    rho = R + r * np.cos(vv)
    V = np.stack([rho * np.cos(uu), rho * np.sin(uu), r * np.sin(vv)], -1).reshape(-1, 3)
    i, j = np.meshgrid(np.arange(n_major), np.arange(n_minor), indexing="ij")
    i1, j1 = (i + 1) % n_major, (j + 1) % n_minor
    idx = lambda a, b: (a * n_minor + b).reshape(-1)  # noqa: E731
    p00, p10, p11, p01 = idx(i, j), idx(i1, j), idx(i1, j1), idx(i, j1)
    F = np.concatenate([np.stack([p00, p10, p11], 1), np.stack([p00, p11, p01], 1)])
    return V, F


def cylinder(radius: float, height: float, n: int) -> tuple[np.ndarray, np.ndarray]:
    """Geschlossener Zylinder (z ∈ [0, h]) mit Fächerdeckeln; F = 4n."""
    a = np.arange(n) * (2.0 * math.pi / n)
    ring = np.stack([radius * np.cos(a), radius * np.sin(a)], -1)
    bot = np.column_stack([ring, np.zeros(n)])
    top = np.column_stack([ring, np.full(n, height)])
    V = np.vstack([bot, top, [[0, 0, 0], [0, 0, height]]])
    k = np.arange(n)
    k1 = (k + 1) % n
    cb, ct = 2 * n, 2 * n + 1
    F = np.concatenate([
        np.stack([k, k1, n + k1], 1), np.stack([k, n + k1, n + k], 1),  # Mantel
        np.stack([np.full(n, cb), k1, k], 1),                           # Boden (−z)
        np.stack([np.full(n, ct), n + k, n + k1], 1),                   # Deckel (+z)
    ])
    return V, F.astype(np.int64)
