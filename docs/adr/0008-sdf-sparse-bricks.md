# ADR 0008 – SDF-Kern: Sparse-Brick-Grid

Status: umgesetzt mit Abweichungen (ADR 0017) · Gate: G6 (M3)

## Entscheidung
- Bricks 8³ Voxel (+1 Voxel Apron für Gradienten/DC), Werte fp16 als
  normierte Distanz f/h, gespeichert nur im Schmalband |f| ≤ 3h.
  fp16-Auflösung bei |f/h| ≤ 3: 2⁻⁹·h ≈ 0,1 µm bei h = 0,05 mm – ausreichend.
- CPU: Brick-Map als offene Hash-Tabelle (Brick-Koordinate → Slot), Slots in
  einem Pool; GPU: Seitentabelle + Brick-Atlas (3D-Textur), Upload nur
  geänderter Bricks.
- Multires: Pyramide mit Faktor 2 (h, 2h, 4h); grobe Pinsel arbeiten auf
  groben Ebenen, Details werden als Residuum gehalten.
- Lokalität: ein Pinselschritt mit Radius R berührt ⌈2R/(8h)⌉³ Bricks; bei
  R = 2 mm, h = 0,05 mm: 10³ = 1000 Bricks, davon im Schmalband typ. ~ 10 %
  → ~100 Bricks · 512 Voxel = 5·10⁴ Voxel-Updates pro Schritt (Abschätzung).
- Remeshing nur der geänderten Bricks + Rand, Dual Contouring auf GPU
  (Compute), Austausch der Mesh-Segmente ohne Stall (ADR 0002 Punkt 3).

## Lesart „20 Mio. Voxel-Bricks im aktiven Bereich“
Angenommen: 2·10⁷ Voxel (≈ 3,9·10⁴ Bricks), Speicher 40 MB fp16.
Wörtliche Lesart 2·10⁷ Bricks = 1,0·10¹⁰ Voxel = 20 GB fp16 (> 12 GB VRAM)
ist interaktiv nicht erreichbar; Alternative wäre Out-of-Core-Streaming mit
deutlich höherer Latenz.

## OpenVDB
Optional (Import/Export `.vdb`, Abgleich), nicht im Kern, wegen
Abhängigkeitsgewicht (TBB, Blosc, Boost-Teile je nach Version).
