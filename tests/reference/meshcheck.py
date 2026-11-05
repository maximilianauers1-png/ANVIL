"""Referenzprüfungen der Druckbarkeit (Spezifikation des C++-Exporters, M1).

Prüft für ein indiziertes Dreiecksnetz (V: n×3 float64, F: m×3 int):
  1. keine degenerierten Dreiecke (skalierungsinvariante Flächenschranke),
  2. jede ungerichtete Kante genau zweimal, mit entgegengesetzter Orientierung
     (⇒ 3F = 2E, geschlossene orientierbare 2-Mannigfaltigkeit entlang Kanten),
  3. Mannigfaltigkeit an Vertices (Fächer um jeden Vertex zusammenhängend),
  4. je Schale: Euler V − E + F = 2(1 − g) mit ganzzahligem g ≥ 0,
  5. je Schale: signiertes Volumen V = (1/6) Σ v0·(v1 × v2) > 0.

Selbstschnitte prüft die C++-Implementierung (Embree-BVH + exakter
Dreieck-Dreieck-Test, M1); diese Referenz liefert dafür keinen Orakelwert.
Jede Verletzung wird mit Index und Ursache gemeldet, nie nur als bool.
"""
from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from tolerances import DEGENERATE_AREA_REL


@dataclass
class Shell:
    faces: int
    edges: int
    vertices: int
    euler: int
    genus: float
    volume: float


@dataclass
class Report:
    ok: bool
    errors: list[str] = field(default_factory=list)
    shells: list[Shell] = field(default_factory=list)
    area: float = 0.0
    volume: float = 0.0


def _components(nv: int, F: np.ndarray) -> np.ndarray:
    """Zusammenhangskomponenten über Kanten (Label-Propagation + Pointer-Jumping)."""
    lab = np.arange(nv, dtype=np.int64)
    while True:
        m = lab[F].min(axis=1)
        new = lab.copy()
        for k in range(3):
            np.minimum.at(new, F[:, k], m)
        # Pointer-Jumping: Label des Labels übernehmen, bis Fixpunkt.
        while True:
            jumped = new[new]
            if np.array_equal(jumped, new):
                break
            new = jumped
        if np.array_equal(new, lab):
            return lab
        lab = new


def check(V: np.ndarray, F: np.ndarray, max_errors: int = 20) -> Report:
    V = np.asarray(V, dtype=np.float64)
    F = np.asarray(F, dtype=np.int64)
    rep = Report(ok=True)

    def err(msg: str) -> None:
        rep.ok = False
        if len(rep.errors) < max_errors:
            rep.errors.append(msg)

    if F.ndim != 2 or F.shape[1] != 3 or len(F) == 0:
        err("face array must be m×3 with m > 0")
        return rep
    if F.min() < 0 or F.max() >= len(V):
        err("face index out of range")
        return rep

    # 1. Degenerierte Dreiecke
    v0, v1, v2 = V[F[:, 0]], V[F[:, 1]], V[F[:, 2]]
    cr = np.cross(v1 - v0, v2 - v0)
    area2 = np.linalg.norm(cr, axis=1)
    diag = float(np.linalg.norm(V.max(0) - V.min(0)))
    thresh = 2.0 * DEGENERATE_AREA_REL * diag * diag
    bad = np.flatnonzero(area2 <= thresh)
    for i in bad[:5]:
        err(f"degenerate triangle #{int(i)} (area {area2[i] / 2:.3e} mm² ≤ {thresh / 2:.3e} mm²)")
    if len(bad) > 5:
        err(f"... {len(bad)} degenerate triangles in total")
    rep.area = float(area2.sum() / 2)

    # 2. Kantenpaarung über gerichtete Halbkanten
    nv = len(V)
    a = np.concatenate([F[:, 0], F[:, 1], F[:, 2]])
    b = np.concatenate([F[:, 1], F[:, 2], F[:, 0]])
    directed = a * nv + b
    order = np.argsort(directed, kind="stable")
    ds = directed[order]
    dup = np.flatnonzero(ds[1:] == ds[:-1])
    for i in dup[:5]:
        k = int(ds[i])
        err(f"half-edge ({k // nv}→{k % nv}) used twice: inconsistent orientation or non-manifold edge")
    lo, hi = np.minimum(a, b), np.maximum(a, b)
    und, cnt = np.unique(lo * nv + hi, return_counts=True)
    for k, c in list(zip(und[cnt != 2], cnt[cnt != 2]))[:5]:
        kind = "boundary (open) edge" if c == 1 else "non-manifold edge"
        err(f"{kind} ({int(k) // nv}–{int(k) % nv}) has {int(c)} incident triangles, expected 2")
    if not np.isin(b * nv + a, directed).all():
        err("some half-edge has no opposite twin")
    E = len(und)
    if 3 * len(F) != 2 * E and rep.ok:
        err(f"3F ≠ 2E ({3 * len(F)} ≠ {2 * E})")

    # 3. Vertex-Mannigfaltigkeit: an einem 2-mannigfaltigen Vertex bilden die
    # Halbkanten genau einen Zyklus. Wir zählen Zyklen der Nachfolger-Abbildung
    # "eingehende Kante (x→v) ↦ ausgehende Kante (v→y) im selben Dreieck".
    if rep.ok:
        used = np.zeros(nv, dtype=bool)
        used[F.reshape(-1)] = True
        # Für Halbkante h = (a→b) im Dreieck: nächste Halbkante um b ist (b→c).
        # Fächer um Vertex v: rotiere über twin(next(h)). Zyklenzahl je v:
        m = len(F)
        nxt = np.concatenate([np.arange(m, 2 * m), np.arange(2 * m, 3 * m), np.arange(0, m)])
        pos = np.searchsorted(ds, b * nv + a)
        twin = order[pos]
        rot = twin[nxt]  # h=(x→v) ↦ (y→v) — nächste eingehende Kante um v
        cycles = np.zeros(nv, dtype=np.int64)
        # Zyklus-Repräsentant = kleinster Index im Zyklus. Nach k Schritten ist
        # rep_min für alle Kanten exakt, deren Zyklus (Vertexvalenz) ≤ k ist;
        # Iterationszahl = maximale Valenz, vollständig vektorisiert.
        start = np.arange(3 * m)
        cur = rot.copy()
        rep_min = start.copy()
        returned = cur == start
        while not returned.all():
            rep_min = np.minimum(rep_min, cur)
            cur = rot[cur]
            returned |= cur == start
        is_rep = rep_min == start
        np.add.at(cycles, b[is_rep], 1)
        nm = np.flatnonzero(used & (cycles != 1))
        for v in nm[:5]:
            err(f"non-manifold vertex #{int(v)} ({int(cycles[v])} triangle fans)")

    # 4./5. Schalen: Euler-Charakteristik und signiertes Volumen
    vol_t = np.einsum("ij,ij->i", v0, np.cross(v1, v2)) / 6.0
    rep.volume = float(vol_t.sum())
    lab = _components(nv, F)
    face_lab = lab[F[:, 0]]
    for s in np.unique(face_lab):
        fs = F[face_lab == s]
        vs = len(np.unique(fs))
        fa, fb = fs[:, [0, 1, 2]].reshape(-1), fs[:, [1, 2, 0]].reshape(-1)
        es = len(np.unique(np.minimum(fa, fb) * nv + np.maximum(fa, fb)))
        chi = vs - es + len(fs)
        g = (2 - chi) / 2
        vol = float(vol_t[face_lab == s].sum())
        rep.shells.append(Shell(len(fs), es, vs, chi, g, vol))
        if g < 0 or g != int(g):
            err(f"shell {len(rep.shells) - 1}: Euler characteristic χ = {chi} is not 2(1−g), g ∈ ℕ")
        if not vol > 0:
            err(f"shell {len(rep.shells) - 1}: signed volume {vol:.6e} mm³ ≤ 0 (inverted normals)")
    return rep
