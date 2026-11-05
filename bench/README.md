# Benchmarks

## Szene BS-5M/1

Generator: `bench/scene/generate_scene.py` (deterministisch, Seed `0xA4F11`).

```
python bench/scene/generate_scene.py --out bench/out
```

| Inhalt | Anzahl | Dreiecke |
|---|---|---|
| Mini-Proxies (Ikosphäre L7, radial verschoben, |a| = 0,35) | 14 | 14 × 327 680 = 4 587 520 |
| Torus R 20 / r 5 mm, δ = 0,5 µm | 1 | 221 662 |
| Torus R 30 / r 3 mm, δ = 0,5 µm | 1 | 197 566 |
| Zylinder-Pins r 3 mm, h 12 mm, δ = 1 µm | 24 | 24 × 488 = 11 712 |
| **Summe** | 40 Objekte | **5 018 460** |

Keine Instanzierung: jedes Dreieck ist eigener Speicher. Alle 40 Objekte
bestehen die Referenzprüfungen (geschlossen, mannigfaltig, Euler, Volumen > 0).
Radiale Verschiebung r(u) = R(1 + s(u)) mit |s| < 1 ist injektiv auf der Sphäre
⇒ beweisbar selbstschnittfrei.

Gemessene Referenzwerte (M0, Linux-Container, 4 vCPU, Python 3.13.18, numpy 2.5.3):

| Größe | Wert |
|---|---|
| STL-Größe | 250 923 084 B = 84 + 50 · 5 018 460 ✓ |
| SHA-256 | `bb5ecc6878d39aa40d202cf1aa2139a79a647f0f1d25a34a7e55002d6dfb1294` (2 Läufe identisch) |
| Torus-Volumenfehler vs. 2π²Rr² | 1,59·10⁻⁴ bzw. 2,40·10⁻⁴ (erwartet ≈ (2π/n)²/6 + Major-Term) |
| Erzeugung inkl. Validierung | 65,8 s; Schreiben 4,1 s |

Hash-Stabilität: Positionen werden auf 2⁻²⁰ mm quantisiert, um
plattformabhängige ulp-Unterschiede von `sin` zu absorbieren. Weicht der Hash
auf einer anderen Plattform ab, sind die Zählwerte und Volumina im
`manifest.json` maßgeblich (Zählwerte exakt, Volumina/Flächen mit
relativer Toleranz 10⁻⁹).

## Kamerapfade (für G1/G6)
1. **Orbit:** 360° um den Szenenschwerpunkt, 1800 Frames, Abstand so, dass die
   gesamte Szene den Viewport füllt.
2. **Zoom:** logarithmisch von Szenengröße auf 5 mm Bildbreite an `mini_03`
   und zurück, 900 + 900 Frames (prüft Tiefenpräzision und Kamera-relative
   Darstellung).
3. **Pan + Hover-Snapping:** Mauspfad über Kanten der Tori (Snap-Kandidaten
   pro Frame aktiv), 1800 Frames.
Je Pfad 3 Läufe, erste 120 Frames Warm-up verworfen; Bericht: p50, p95, p99,
max, Histogramm, GPU-Zeit je Pass.

## Berichtsformat
`bench/reports/Mx.md` + `Mx.json` mit Hardware (GPU, Treiber, CPU, RAM,
Auflösung, Bildfrequenz), Build-Hash, Messwerten, Gate-Status.
