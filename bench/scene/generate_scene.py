"""ANVIL-Benchmark-Szene "BS-5M" (deterministisch, ≥ 5 000 000 Dreiecke, keine Instanzierung).

Zusammensetzung (spiegelt beide Einsatzzwecke):
  * 14 "Mini-Proxies": Ikosphären Stufe 7 (je 327 680 Dreiecke), radial mit
    glattem, deterministischem Rauschen verschoben. Radiale Verschiebung mit
    |a| < 1 hält die Abbildung injektiv ⇒ beweisbar selbstschnittfrei.
  * 2 Tori, tesselliert per Sehnenfehler δ = 0.5 µm (Laborteile, lange dünne
    Dreiecke — Worst Case für Rasterisierung und Picking).
  * 24 Zylinder (Lochraster-Bolzen), δ = 1 µm.
Jedes Objekt wird mit meshcheck validiert. Ausgabe: binäre STL der Gesamtszene
und manifest.json (Zählwerte, Volumina, SHA-256).

Aufruf:  python bench/scene/generate_scene.py --out bench/out
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests" / "reference"))

import meshcheck  # noqa: E402
import primitives as P  # noqa: E402
import stl  # noqa: E402

SEED = 0xA4F11  # fest; Änderung ⇒ neue Szenenversion
SCENE_VERSION = "BS-5M/1"
QUANT_MM = 2.0**-20  # Quantisierung (~0.95 nm) für plattformstabile Hashes


def displaced_sphere(rng: np.random.Generator, level: int, radius: float, amp: float, waves: int):
    V, F = P.icosphere(level, 1.0)
    d = rng.normal(size=(waves, 3))
    d /= np.linalg.norm(d, axis=1, keepdims=True)
    freq = rng.uniform(2.0, 18.0, waves)
    phase = rng.uniform(0, 2 * math.pi, waves)
    w = rng.uniform(0.3, 1.0, waves) / freq  # 1/f-Spektrum ⇒ "organisch"
    s = np.sin((V @ d.T) * freq + phase) @ w
    s *= amp / np.abs(w).sum()  # |s| ≤ amp < 1 ⇒ injektiv
    return V * (radius * (1.0 + s))[:, None], F


def quantize(V: np.ndarray) -> np.ndarray:
    return np.round(V / QUANT_MM) * QUANT_MM


def build(validate: bool = True):
    rng = np.random.default_rng(SEED)
    objs = []
    for i in range(14):
        V, F = displaced_sphere(rng, 7, radius=14.0, amp=0.35, waves=48)
        objs.append((f"mini_{i:02d}", V + [(i % 7) * 40.0, (i // 7) * 40.0, 16.0], F, None))
    for j, (R, r) in enumerate(((20.0, 5.0), (30.0, 3.0))):
        nr, nR = P.segments_for_chord(r, 5e-4), P.segments_for_chord(R + r, 5e-4)
        V, F = P.torus(R, r, nR, nr)
        objs.append((f"torus_{j}", V + [j * 80.0, -60.0, r], F, 2 * math.pi**2 * R * r**2))
    for k in range(24):
        n = P.segments_for_chord(3.0, 1e-3)
        V, F = P.cylinder(3.0, 12.0, n)
        objs.append((f"pin_{k:02d}", V + [(k % 6) * 25.0, -120.0 - (k // 6) * 25.0, 0.0], F, None))

    manifest = {"version": SCENE_VERSION, "seed": SEED, "unit": "mm", "objects": []}
    for name, V, F, v_exact in objs:
        V = quantize(V)
        entry = {"name": name, "triangles": int(len(F)), "vertices": int(len(V))}
        if validate:
            rep = meshcheck.check(V, F)
            if not rep.ok:
                raise RuntimeError(f"{name}: {rep.errors}")
            entry |= {"volume_mm3": rep.volume, "area_mm2": rep.area,
                      "genus": [s.genus for s in rep.shells]}
            if v_exact is not None:
                entry["volume_rel_err_vs_analytic"] = abs(rep.volume - v_exact) / v_exact
        manifest["objects"].append(entry)
    manifest["triangles_total"] = sum(o["triangles"] for o in manifest["objects"])
    return objs, manifest


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=Path("bench/out"))
    ap.add_argument("--no-validate", action="store_true")
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    t0 = time.perf_counter()
    objs, man = build(validate=not a.no_validate)
    t1 = time.perf_counter()
    recs = np.concatenate([stl.to_records(quantize(V), F) for _, V, F, _ in objs])
    path = a.out / "bs5m.stl"
    man["stl_sha256"] = stl.write_binary(path, recs, note=SCENE_VERSION)
    man["stl_bytes"] = path.stat().st_size
    assert man["stl_bytes"] == 84 + 50 * man["triangles_total"]
    man["generator"] = {"numpy": np.__version__, "python": sys.version.split()[0],
                        "build_s": round(t1 - t0, 2), "write_s": round(time.perf_counter() - t1, 2)}
    man["manifest_sha256_of_counts"] = hashlib.sha256(
        json.dumps([(o["name"], o["triangles"]) for o in man["objects"]]).encode()).hexdigest()
    (a.out / "manifest.json").write_text(json.dumps(man, indent=2))
    print(json.dumps({k: man[k] for k in ("version", "triangles_total", "stl_bytes", "stl_sha256", "generator")}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
