# ADR 0017 – M3: Sculpting auf Sparse-Brick-SDFs (Umsetzung und Abweichungen)

Status: umgesetzt (M3) · Messwerte: `bench/reports/M3.md` · baut auf ADR 0008 auf

## Entscheidungen

| # | Entscheidung | Begründung / Messung |
|---|---|---|
| 1 | Bricks 8³, f/h als binary16 im Band \|f\| ≤ 3h, Bricks per `shared_ptr` geteilt (Copy-on-Write); unbelegte Innen-Bricks in einer Menge `interior` | Ein `Grid`-Snapshot kostet nur die Map-Kopie (1,1 ms bei 4 000 Bricks). Undo-Zustände und die Live-Vorschau teilen alle unberührten Bricks. |
| 2 | **Ein Strich = ein Python-Befehl** `sculpt(obj, brush, path, radius, strength)`; der Knoten `Sculpt` speichert Quelle, Voxelgröße und Striche | Anforderung 2.5 (jede UI-Aktion ein Befehl, ein Undo-Schritt) ohne Journal-Flut. Wiedergabe ist deterministisch; Speichern/Laden ist bitidentisch (Abnahmetest). |
| 3 | Präfix-Cache der Grids über eine Hash-Kette H_k = SHA-256(H_{k−1} \| Strich k) | Ein neuer Strich kostet einen Strich, keine Wiedergabe der Historie. |
| 4 | Live-Striche: `StrokeApplier` auf einem Worker-Thread, dieselbe Klasse bei der Wiedergabe; das Ergebnis wird vor dem Befehl in den Cache gelegt | Live und Wiedergabe sind bitidentisch (Unit-Test), der Befehl rechnet den Strich nicht noch einmal. |
| 5 | **Abweichung von ADR 0008/M0 §3.7:** Zellvertex = Schwerpunkt der Kantenschnitte (Surface-Nets-Platzierung), nicht das QEF-Minimum | Gemessen an den Abnahme-Strichen: QEF (σ-Abschnitt 0,1, auf Zelle geklemmt) → 294 Selbstschnitte + 8 entartete Dreiecke; QEF mit Verschiebung ≤ 0,35 h → 7; Schwerpunkt → 0. Ein Export muss die Prüfungen bestehen (Anforderung 2.4). Folge: scharfe Kanten werden um höchstens eine Zelle gebrochen (|ΔV| ≤ h²/2 · Kantenlänge, Test). Exakte Kanten entstehen weiterhin über Booleans mit CAD-Körpern auf dem Ausgabenetz. Eine schnittfreie QEF-Variante (Manifold DC mit Vertex-Splitting) bleibt Kandidat für M5. |
| 6 | Smooth-Pinsel diffundiert den geklemmten Indikator c = clamp(f/h, −1, 1) (MBO-Schema, Merriman/Bence/Osher 1992) | Mitteln von f selbst schärfte konvexe Ecken (Mittelachse des SDF im Inneren); gemessen: Eckenabstand 0,165 → 0,131 statt größer. MBO bewegt die Nullfläche mit der mittleren Krümmung. |
| 7 | Reinitialisierung: f ← f/\|∇f\| nur für \|f\| < 1,5 h mit ungeklemmtem Stencil, äußeres Band als 26-Nachbar-Chamfer-Distanz (Borgefors 1986), nur im Pinselbereich | Ohne Schritt 2 driftete der Bandrand pro Dab nach innen, bis Kreuzungen verworfen wurden (Löcher im Netz, gefunden im Test). |
| 8 | Grab: semi-Lagrangesche Advektion mit Teilschritten ≤ R/4 | Injektivität von x ↦ x + w d verlangt \|d\|·max\|w′\| < 1; Wendland C2: max\|w′\| = 2,11/R ⇒ \|d\| < 0,47 R; R/4 lässt Faktor 1,9 Reserve. Grab p50 99 → 31 ms. |
| 9 | Meshing in Chunks (4³ Bricks); Chunk-Schlüssel aus **Netz-Stempeln** der Bricks, die sich nur bei Änderungen mit min(\|alt\|,\|neu\|) < 2,5 h ändern | Kontur nutzt nur Kanten mit \|a\|+\|b\| < 2,5 ⇒ exakt; Bandpflege löst kein Neu-Meshing aus. Nur geänderte Chunks werden neu vernetzt und hochgeladen. |
| 10 | Eigene Chunk-Kontur (27-Nachbar-Brick-Tabelle, dichtes Zellindex-Feld) | Vollständige Kontur 4,36 M Dreiecke: 2 692 → 234 ms (× 11,5). |
| 11 | Sculpt-Formen tragen keinen `Manifold`; er wird für Booleans/Deform bei Bedarf gebaut | Ein Manifold für Millionen Dreiecke pro Strich wäre teurer als der Strich. |
| 12 | Exportprüfung auf float32-Daten: Toleranzboden 16 ulp der größten Koordinate; symmetrische Koplanarität; schlecht konditionierte Schnittlinien (r/sin θ ≥ 1 % L) werden in der Ebene entschieden | Gefunden an M3, betrifft aber auch reines CAD: Kugel − Bohrung Ø 4,01 mm meldete 4 Selbstschnitte, die nur durch die float-Rundung berührender Splitter entstehen. 16 ulp bei 50 mm = 0,1 µm ≪ 10 µm Sehnenfehler ≪ 18 µm Druckerpixel; Slicer rechnen in 1-nm-Ganzzahlen. Echte Selbstschnitte bleiben erkannt (bestehende Tests). |
| 13 | Voxelgröße Standard 50 µm, Untergrenze 10 µm, Obergrenze 10¹⁰ Voxel in der Bounding-Box (`tolerance.hpp`) | Sagitta κh²/8 = 3 µm bei 0,1 mm Radius; feiner als die halbe Druckerpixelgröße bringt nichts. |

## Folgen

- G6 ist in der Agentenumgebung nur für den CPU-Teil messbar (`anvil-sculpt-bench`);
  der GPU-Teil (Upload, Zeichnen in BS-5M) wird auf dem Zielsystem mit
  `scripts/run-benchmarks.ps1` gemessen.
- GPU-Dual-Contouring (ADR 0008 Punkt 4) ist nicht umgesetzt: der CPU-Pfad
  erreicht nach den Optimierungen 8 ms Neu-Meshing pro Frame auf 4 vCPU.
