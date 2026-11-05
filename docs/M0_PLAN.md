# ANVIL – M0: Architektur- und Umsetzungsplan

Stand: 2026-11-05 · Status: **freigegeben**; M1 umgesetzt (siehe ADR 0014/0015, `bench/reports/M1.md`)

Dieses Dokument ist die verbindliche Planungsgrundlage. Entscheidungen mit
Begründung stehen in `docs/adr/`, Risiken in `docs/RISIKEN.md`, Abhängigkeiten
in `THIRD_PARTY.md`, das Benchmark-Protokoll in `bench/README.md`.

---

## 0. Was M0 liefert und was nicht

| Geliefert | Nachweis |
|---|---|
| Architektur, Prozessmodell, Datenfluss | §2–§4, ADR 0001–0013 |
| Risikoregister mit Gate-Benchmarks | `docs/RISIKEN.md` |
| Benchmark-Szene BS-5M, deterministisch | `bench/scene/generate_scene.py`, 5 018 460 Dreiecke, SHA-256 reproduziert |
| Referenzimplementierung der Exportprüfungen (Orakel für C++) | `tests/reference/`, 13 Tests grün |
| Repo-Struktur, `CLAUDE.md`, Lizenzinventar | dieses Repo |
| Rückfragen (≤ 5) | §9 |

**Nicht** geliefert (und nicht behauptet): Leistungsmessungen auf dem Zielsystem.
M0 läuft in einem Linux-Container ohne GPU. Alle Latenz-/VRAM-Zahlen unten sind
**Abschätzungen** und als solche markiert; sie werden in M1 durch die
Gate-Benchmarks G1–G6 (`docs/RISIKEN.md`) ersetzt. Wird ein Ziel verfehlt,
melde ich Messwert, Ursache und Alternative – das Ziel wird nicht aufgeweicht.

---

## 1. Explizite Annahmen

| # | Annahme | Folge, falls falsch |
|---|---|---|
| A1 | RTX 5070 = GB205, 12 GB GDDR7, 672 GB/s, Compute Capability 12.0 (sm_120). Wird beim Start per `nvidia-smi --query-gpu=name,memory.total,driver_version,compute_cap --format=csv,noheader` **und** Vulkan-Geräteabfrage geprüft; Vulkan ist maßgeblich. | VRAM-Budget (ADR 0009) skaliert automatisch; Warnung in der Statusleiste. |
| A2 | Ein Monitor, 2560×1440 @ ≥ 60 Hz. Das 16,7-ms-Ziel ist auflösungsabhängig (Fragment-Last). | Rückfrage F3. |
| A3 | „20 Mio. Voxel-Bricks im aktiven Bereich“ bedeutet **20 Mio. Voxel** in aktiven Bricks (≈ 3,9·10⁴ volle 8³-Bricks). 20 Mio. *Bricks* à 512 Voxel wären 1,0·10¹⁰ Voxel ≈ 20 GB in fp16 nur für Distanzwerte – nicht interaktiv. | Rückfrage F4. |
| A4 | „Lineare Tet-FEM“ = lineare Elastizität. Elementtyp T10 (quadratisch) als Default, weil T4 bei Biegung sperrt (ADR 0011). | Bei Wunsch T4: Validierung nur mit feinem Netz und dokumentiertem Fehler. |
| A5 | Nutzung privat, nicht kommerziell, Standort Schweden (EU). Maßgeblich für Modell-Lizenzen (ADR 0012). | Lizenzmatrix neu bewerten. |
| A6 | Windows 11 Pro x64, MSVC 2022 (v143) oder neuer, Visual Studio Build Tools vorhanden oder installierbar. | — |
| A7 | Python 3.11 bleibt Vorgabe (Security-Support bis Okt. 2027). ANVIL bringt sein eigenes eingebettetes CPython 3.11 mit; die System-Python-Installation wird nicht verändert. | Migration auf 3.12+ per ADR. |
| A8 | Ollama läuft separat; ANVIL startet es nicht, prüft aber Bind-Adresse und Erreichbarkeit nur auf 127.0.0.1. | — |

---

## 2. Systemübersicht

```
┌──────────────────────────── anvil.exe (Hauptprozess) ────────────────────────────┐
│  Qt Quick UI (QML, Design Tokens)  ── Input-Thread: Radialmenü-Gestenerkennung   │
│        │                                                                          │
│  Render-Thread (Qt RHI/Vulkan, eigenes VkDevice geteilt)                          │
│        │  Viewport-Pass (native Vulkan-Kommandos)  · Async-Compute-Queue          │
│  Document Core (C++20)                                                            │
│    Feature-DAG ◄── Command Bus ◄── { UI-Aktionen, Python, KI-Transaktionen }      │
│    Transaktionsjournal (append-only, fsync)  · Autosave-Snapshots                 │
│    Mesh-Kernel: Manifold · Embree · SDF-Bricks · Exporter-Prüfungen               │
│  Embedded CPython 3.11 (eigener Thread, pybind11, Modul `anvil`)                  │
└──────┬──────────────────────────────┬────────────────────────────┬───────────────┘
       │ Named Pipe + Shared Memory   │                            │
┌──────▼──────────┐   ┌───────────────▼─────────────┐   ┌──────────▼──────────────┐
│ anvil-geom.exe  │   │ anvil-ai.exe (Python 3.11)   │   │ anvil-sandbox.exe       │
│ OpenCASCADE,    │   │ Keras 3 + PyTorch cu128,     │   │ AppContainer, kein Netz,│
│ STEP/3MF/OBJ-   │   │ ONNX Runtime, Ollama-Client  │   │ nur Projektordner;      │
│ Import, Repair, │   │ (127.0.0.1), VRAM-Manager-   │   │ führt KI-Python gegen   │
│ FEM-Solver      │   │ Client, Training             │   │ Dokument-Proxy aus →    │
│ (absturzisoliert)│  │ (absturzisoliert)            │   │ liefert Transaktion     │
└─────────────────┘   └──────────────────────────────┘   └─────────────────────────┘
```

Kernprinzipien:

1. **Ein Befehlsbus.** Jede Zustandsänderung ist ein `Command` mit kanonischer
   Python-Darstellung. UI-Klick → `Command` → Journal → DAG. Ein Befehl = ein
   Undo-Schritt; ein Python-Block in der Command Bar wird als *eine* Transaktion
   gebündelt (ADR 0004/0005).
2. **Crash-Isolation nach Absturzwahrscheinlichkeit.** Code mit bekannter
   Absturzgefahr auf fremden Eingaben (OCCT-Booleans/Fillets, STEP-Import,
   KI-Inferenz, KI-Code) läuft außerhalb des Hauptprozesses. Manifold, Embree
   und der SDF-Kern laufen im Hauptprozess, weil sie latenzkritisch sind und
   gegen garantierte Mannigfaltigkeit arbeiten (ADR 0001).
3. **Kein Arbeitsverlust.** Das Journal wird *vor* der Ausführung eines Befehls
   per `FlushFileBuffers` persistiert; Recovery = letzter Snapshot + Replay.
   Stürzt der Hauptprozess ab, verliert man höchstens den laufenden Befehl.
4. **Render-Thread blockiert nie.** Er wartet ausschließlich auf vsync. Geometrie
   wird über Timeline-Semaphoren und doppelt gepufferte GPU-Puffer übergeben.

---

## 3. Subsysteme (Kurzfassung, Details in ADRs)

### 3.1 Renderer (ADR 0007)
- Vulkan 1.3 Core: Dynamic Rendering, Synchronization2, Timeline-Semaphoren,
  Buffer Device Address. Optional Mesh Shader (`VK_EXT_mesh_shader`) mit
  Meshlet-Culling; Fallback klassischer Vertex-Pfad.
- Kamera-relative Darstellung: Positionen intern `double` (mm); pro Objekt wird
  `T_obj→cam` in double gebildet und erst dann nach `float` gerundet. Damit ist
  der Positionsfehler im Bild ≈ |x−c|·2⁻²⁴ statt |x|·2⁻²⁴.
- Reversed-Z mit unendlicher Fernebene (`D32_SFLOAT`) – gleichmäßige
  Tiefenpräzision über 5 Größenordnungen (0,01 mm Details bis 1 m Szene).
- Picking: ID-Puffer (R32_UINT) + Embree-Raycast für Sub-Pixel-Snaps.
- **Abschätzung (unbelegt bis G1):** 5 M Dreiecke ≈ 2,5 M Vertices.
  Oktaeder-Normalen (4 B) + Position fp32 (12 B) = 40 MB, Indizes 60 MB
  → 100 MB/Frame Lesen ≈ 0,15 ms bei 672 GB/s. Die Primitive-Rate des GB205
  liegt im Bereich mehrerer 10⁹ Dreiecke/s, also ≈ 1–2 ms Geometrie. Das Risiko
  liegt nicht im Rasterizer, sondern in CPU-Synchronisation, Qt-Integration und
  Overlays (Kanten, Selektion, SSAO) → Frame-Budget in ADR 0007.

### 3.2 UI (ADR 0002)
- Qt 6 Quick, dynamisch gelinkt (LGPLv3). Qt Quick und Viewport teilen **ein**
  `VkDevice`, das ANVIL selbst erzeugt und Qt via
  `QQuickGraphicsDevice::fromDeviceObjects` übergibt. Der Viewport rendert als
  native Kommandos in Qts Render-Pass (`beforeRenderPassRecording`) – keine
  Kopie, keine zusätzliche Latenz.
- QML wird mit `qmlcachegen` vorab kompiliert (Kaltstart).
- Design Tokens als `QtObject`-Singleton; Dark/Light per Property-Bindung live.

**Radialmenü (Marking Menu).**
- Sektorwahl über Winkel: Sektor `k = ⌊(θ + π/N) / (2π/N)⌋ mod N`, N ≤ 8,
  θ = atan2(−Δy, Δx), Sektor 0 = Osten, gegen den Uhrzeigersinn. Totzone
  `‖Δ‖ < r₀` (Default r₀ = 12 px · DPI-Faktor) → Abbruch.
- Fitts: Bei Winkelauswahl ist die effektive Zielbreite in Abstand D gleich
  W = 2πD/N; also ID = log₂(1 + D/W) = log₂(1 + N/2π) = **1,18 bit für N = 8,
  unabhängig von D**. Deshalb skaliert das Menü nicht mit der Bewegungsdistanz;
  große Flicks sind nicht teurer.
- Flicks vor dem ersten Zeichnen: Die Geste wird im Input-Pfad aus den
  zeitgestempelten Rohereignissen ausgewertet (Taste down → Mauspfad → Taste up),
  nicht aus dem gezeichneten Menü. Ein Release vor dem ersten Present löst
  den Befehl trotzdem aus. Das Menü wird dennoch im nächsten Frame gezeichnet
  (Ziel ≤ 1 Frame): das QML-Item ist vorinstanziiert, nur `visible` wechselt.
- Mehrstufig: Verweilen > τ_hover (Default 250 ms) oder Richtungswechsel > 60°
  öffnet den Unterring (Hierarchical Marking Menu, Kurtenbach & Buxton 1993).
- Stabilität: Jeder Befehl hat einen festen Slot (Modus × Kontext × Ring × Sektor).
  Nicht anwendbare Befehle werden ausgegraut, **nicht entfernt** → kein
  Umsortieren, Muskelgedächtnis bleibt. Layout editierbar per Drag & Drop,
  gespeichert als JSON im Profil.

**Command Bar.** Ein Textfeld, drei Rollen: Fuzzy-Suche (Smith-Waterman-artige
Teilfolgenbewertung mit Bonus für Wortanfänge und Nutzungshäufigkeit, Index
aller registrierten `Command`s), `>` Python (Jedi-Completion im Python-Thread,
Inline-Fehler aus `ast.parse`/Ausführung, Geometrie-Vorschau als Ghost),
`?` KI → erzeugt Python, das erst nach Diff + Ghost-Vorschau angenommen wird.

### 3.3 Snapping
Kandidaten i mit Weltpunkt xᵢ, Projektion π(xᵢ) in Pixel, Mausposition m:

  Jᵢ = ‖π(xᵢ) − m‖ / rᵢ + βᵢ, Kandidat nur gültig für ‖π(xᵢ) − m‖ ≤ rᵢ.

rᵢ = typabhängiger Fangradius (px, DPI-skaliert), βᵢ = Priorität (kleiner =
bevorzugt): Vertex 0,00 · Schnittpunkt 0,05 · Kantenmitte 0,10 · Zentrum 0,10 ·
Schwerpunkt 0,15 · Tangente/Lot 0,20 · Flächennormale 0,30 · Achsen (X/Y/Z,
26 ViewCube-Richtungen) 0,35 · Gitter 0,50. Hysterese: Wechsel von a nach b
nur, wenn J_b < J_a − Δ (Default Δ = 0,15). Alle Werte sind Tokens, keine
Literale im Code. Kandidatensuche: Embree-Raycast + Bildschirmraum-Grid der
projizierten Features im Umkreis max rᵢ, O(k) je Mausbewegung. Alt hebt
Snapping auf, solange gedrückt. Aktiver Snap: farbiges Glyph + Label
(„Midpoint“, „Tangent“ …).

ViewCube: 26 Richtungen = 6 Flächen + 12 Kanten + 8 Ecken, d.h. alle
d ∈ {−1,0,1}³ \ {0}, normiert. Dieselbe Menge dient als Achsen-Snap.

### 3.4 Dokumentmodell (ADR 0004)
Feature-DAG mit unveränderlichen, inhaltsadressierten Knotenergebnissen
(Hash über Parameter + Eingangs-Hashes) → Neuberechnung nur des betroffenen
Teilgraphen. Persistenz: `.anvil` = ZIP mit `document.json` (DAG),
`journal.log`, `cache/` (optionale Ergebnisse), `assets/`.

### 3.5 Einheiten und Numerik (ADR 0006)
- C++: `Quantity<Dim>` mit Dimensionsexponenten (L, M, T, Θ, I, Winkel) zur
  Compile-Zeit; intern double in mm, kg, s, K, A, rad.
- Python: Laufzeit-`Quantity`, `mm`, `cm`, `m`, `inch`, `deg` als Konstanten.
- Ausdrucksparser (C++, Pratt) für Eingabefelder: `12.5 cm + 3 mm` → 128 mm;
  `2*r` mit benannten Variablen; Dimensionsfehler präzise gemeldet
  („cannot add Length and Angle“).
- Toleranzen zentral in drei Ebenen (Numerik, Kernel, Fertigung), Referenz in
  `tests/reference/tolerances.py`.
- Testfall 25,4 mm ↔ 1 inch: Zollfaktor ist per Definition exakt 25,4;
  Roundtrip über 10⁴ Zufallswerte innerhalb 16 ulp (Referenztest grün).

### 3.6 Präzisions-CAD
- Sketch-Solver: Levenberg-Marquardt, (JᵀJ + λD)Δq = −JᵀC mit
  **Marquardt-Skalierung** D = diag(JᵀJ), weil q Längen (mm) und Winkel (rad)
  mischt; reines λI wäre einheitenabhängig. Gelöst über QR von
  [J; √λ·D^½] (numerisch stabiler als Normalgleichungen, κ statt κ²).
- Freiheitsgrade: SVD J = UΣVᵀ am Lösungspunkt, rang r = #{σᵢ > τ·σ₁},
  τ = 10⁻⁹. DOF = n − r. Unterbestimmte Variablen: Zeilen von V_null (letzte
  n − r Spalten von V) mit Norm > 10⁻⁶ → blau markiert. Überbestimmung:
  m > r ⇒ redundante Constraints; Zuordnung über den linken Nullraum U_null
  (Constraints mit großem Gewicht dort) → rot, falls ‖C‖ > Toleranz
  (Widerspruch), orange, falls konsistent-redundant.
- B-Rep-Operationen (Extrude, Revolve, Sweep, Loft, Shell, Fillet, Chamfer,
  Pattern, Booleans) über OCCT im Geometrie-Worker.

### 3.7 Organik und SDF (ADR 0008)
- Sparse-Brick-Grid: Bricks 8³ Voxel, Distanz als fp16 im Schmalband
  |f| ≤ 3h; Brick-Lookup per zweistufiger Hash-/Baumstruktur (CPU) und
  GPU-Seitentabelle (Compute). Speicherbedarf bei A3: 20 M Voxel × 2 B = 40 MB.
- Glatte Vereinigung: h = max(k − |a−b|, 0)/k, smin = min(a,b) − h²k/4.
  Eigenschaft (nachgerechnet): ∂smin/∂a = 1 − h/2, ∂smin/∂b = h/2 (für a<b),
  beide ∈ [0,1], Summe 1 ⇒ ‖∇smin‖ ≤ max(‖∇a‖,‖∇b‖), d.h. **Lipschitz-Schranke
  bleibt erhalten**; maximaler Fehler gegenüber min ist k/4 bei a = b. smin ist
  keine exakte SDF (unterschätzt) – für Sphere Tracing unschädlich.
- Sphere Tracing: t_{n+1} = t_n + f(p)/L; L wird je Knoten im SDF-Graphen
  propagiert (Skalierung s ⇒ L·s, Verschiebung ⇒ L·max‖∇d‖).
- Eikonal-Reinitialisierung per Fast Sweeping (Zhao 2005), ausgelöst wenn
  mittleres | ‖∇f‖ − 1 | im Brick > 0,1.
- Meshing: Manifold Dual Contouring (Schaefer, Ju, Warren 2007). QEF
  x* = argmin Σ (nᵢ·(x − pᵢ))² über SVD mit Abschneiden σᵢ < 0,1·σ₁ und
  Regularisierung zum Massenpunkt der Kantenschnitte; Clamping in die Zelle.
- Voxelgröße: Sagitta-Fehler δ ≈ κh²/8. Beispiel h = 0,05 mm, kleinster
  Krümmungsradius 0,1 mm (κ = 10 mm⁻¹): δ = 3,1 µm ≪ Resin-XY-Pixel (≈ 20–35 µm).
  Begrenzend ist nicht δ, sondern die **Darstellbarkeit**: Strukturen < 2h
  (0,1 mm) verschwinden → Detail-Linter prüft genau diese Grenze.
- Dreieckszahl einer 32-mm-Mini bei h = 0,05 mm: N ≈ 2A/h²; mit A ≈ 4000 mm²
  folgt N ≈ 3,2 M – innerhalb des 5-M-Viewportziels; STL ≈ 160 MB. Dezimierung
  für den Export optional, fehlergesteuert (Hausdorff ≤ δ_print).
- Sculpting: Δx = s·w(q)·n̂, Wendland-C2 w(q) = (1−q)⁴(1+4q), q = ‖x−c‖/R.
  Auf der SDF als Δf = −s·w(q) im Schmalband, danach lokale Reinitialisierung
  und Neu-Meshing nur betroffener Bricks (+1 Brick Rand).

### 3.8 Mini Forge
Armature (Knochen + Kapsel-/Ellipsoid-SDF-Muskeln, `smin`-verschmolzen),
Dual-Quaternion-Skinning (Kavan 2008) – kein Candy-Wrapper-Effekt,
XPBD (Macklin 2016) mit Compliance α̃ = α/Δt² für Stoff, danach Einfrieren,
Ausdicken als SDF-Offset. Maßstab: s = Zielhöhe / reale Höhe; 1,80 m auf
32 mm ⇒ 1:56,25. Displacement-Bibliothek prozedural (Kettenhemd, Schuppen,
Nieten, Runen …) als SDF-Operatoren. Nur eigene/prozedurale Assets.

### 3.9 Druck und Export
- Exporter-Prüfungen exakt wie `tests/reference/meshcheck.py` (Orakel):
  Kantenpaarung mit Gegenorientierung (⇒ 3F = 2E), Vertex-Mannigfaltigkeit,
  Euler je Schale, signiertes Volumen > 0, degenerierte Dreiecke,
  Selbstschnitte (Embree-BVH + exakter Dreieck-Dreieck-Test mit
  Shewchuk-Prädikaten). Jede Verletzung mit Index, Ort und Ursache.
- Binär-STL: 84 + 50·N Byte (Referenztest grün); Einheit im Header und im Dialog.
- Tesselierung: n = ⌈π / arccos(1 − δ/r)⌉; Beispiel r = 12,5 mm, δ = 0,01 mm
  ⇒ n = 79 (Test: n erfüllt δ, n−1 verletzt δ).
- Reparatur: Generalized Winding Number (Jacobson 2013), Raumwinkel je Dreieck
  nach Van Oosterom–Strackee, tan(Ω/2) = a·(b×c) / (abc + (a·b)c + (b·c)a + (c·a)b),
  hierarchisch beschleunigt (Barill 2018); innen bei w > ½; danach
  Rekonstruktion via SDF + Dual Contouring.
- Overhang-Konvention: b = Baurichtung (Einheitsvektor), α_c = kritischer
  Winkel **gemessen gegen die Vertikale**. Kritisch, wenn n·(−b) > sin α_c
  (äquivalent: Neigung der Fläche gegen die Vertikale > α_c). Die Konvention
  wird im Profil-Dialog angezeigt.
- Orientierung: Startmenge gleichverteilt auf SO(3) (Super-Fibonacci-Spiralen,
  Alexa 2022), dann CMA-ES auf Quaternionen (Normierung nach jedem Schritt,
  q ≡ −q), J(R) = w₁A_support + w₂ max_z A(z) + w₃N_islands + w₄H.

### 3.10 Physik-Checks (M5)
ΔL = α·L·ΔT; Hochspannung U/d > E_max/S und Kriechstrecke (IEC 60664-1:
benötigt Verschmutzungsgrad und Materialgruppe/CTI – Nutzereingabe, keine
Defaults). FEM: T10-Tetraeder, K·u = f, PCG mit AMG-Vorkonditionierer
(AMGCL); Validierung Kragbalken mit Timoshenko-Korrektur (ADR 0011).

### 3.11 KI (ADR 0009, 0010, 0012)
Keras 3 mit PyTorch-Backend (cu128-Wheels, sm_120), ONNX Runtime als
Alternative, TensorFlow nur über WSL2. `.keras`-Dateien werden ausschließlich
im KI-Worker mit `safe_mode=True` geladen (Lambda-Deserialisierung =
Codeausführung). Ollama nur über 127.0.0.1, typisierte Tools mit JSON-Schema.
VRAM-Manager: Viewport-Reserve zuerst, LRU, max. ein großes Modell.

---

## 4. Repo-Struktur

```
ANVIL/
├── CLAUDE.md               Arbeitsregeln für Agenten (verbindlich)
├── README.md               Überblick (de)
├── THIRD_PARTY.md          Versionen, Lizenzen, Prüfstatus
├── CMakeLists.txt          (M1) Superbuild-Einstieg, CMakePresets.json, vcpkg.json
├── core/                   (M1) C++20: units, tolerance, command, doc (DAG, journal),
│                                 mesh, export, snapping, expr-parser
├── kernel/                 (M1–M3) manifold_bridge, embree_bridge, sdf/, sculpt/
├── render/                 (M1) Vulkan-Renderer, shaders/ (GLSL → SPIR-V, offline)
├── ui/                     (M1) QML, tokens/, radial/, commandbar/, viewcube/
├── python/anvil/           (M1) Python-API (öffentlich), _anvil (pybind11)
├── workers/
│   ├── geom/               (M2) anvil-geom.exe: OCCT, STEP, FEM
│   ├── ai/                 (M6) anvil-ai (Python): Keras/Torch, ONNX, Ollama
│   └── sandbox/            (M6) anvil-sandbox.exe: AppContainer-Host
├── ipc/                    (M1) Named-Pipe-Protokoll, Shared-Memory-Ringpuffer
├── plugins/                Python-Plugins: lab/, miniforge/, brushes/, generators/
├── bench/
│   ├── README.md           Benchmark-Protokoll, Gates
│   ├── scene/              BS-5M-Generator (vorhanden)
│   └── reports/            Benchmark-Berichte je Meilenstein (Markdown + JSON)
├── tests/
│   ├── reference/          Python-Orakel (vorhanden, grün)
│   ├── unit/ property/ golden/ latency/   (M1)
├── docs/                   Deutsch: M0_PLAN.md, RISIKEN.md, adr/, handbuch/
└── tools/                  hwcheck, license-audit, offline-check
```

---

## 5. Meilensteinplan

| M | Inhalt | Abnahme | Gates |
|---|---|---|---|
| M1 | Fenster, Viewport, Themes, Radialmenü, Command Bar, Python, Einheiten, Undo, Primitive, Manifold-Booleans, Snapping, STL-Export | Magnetbasis per UI und per Skript → identische Geometrie (bitgleiche STL-Hashes), alle Prüfungen bestanden | G1 Viewport p99, G2 Radialmenü ≤ 1 Frame, G3 Kaltstart < 3 s, G4 Qt/Vulkan-Integration |
| M2 | Sketcher, LM-Solver, OCCT-Worker, STEP, Timeline | Parametrische Klemme; Maßänderung propagiert; Constraint-Status korrekt | G5 Worker-Crash-Isolation |
| M3 | SDF, Dual Contouring, Sculpting | Glatter Viewport beim Sculpten in BS-5M | G6 Sculpt-Latenz |
| M4 | Mini Forge | 32-mm-Dämon mit Basis < 30 min, druckfertig | – |
| M5 | Print-Modus, Lab-Bibliothek, FEM | Kragbalken-Validierung, Overhang/Insel-Heatmaps | – |
| M6 | KI, Neural Lab | Keras-Modell trainieren → Knoten per D&D → valide STL | VRAM-Budget |
| M7 | Härtung | Installer, Crash-/Leak-/Offline-Tests, Handbuch | alle Gates erneut |

Nach jedem Meilenstein: Tests grün, Benchmark-Bericht (`bench/reports/Mx.md`),
Commit, kurze Demo-Anleitung.

**Arbeitsteilung mit Ihrer Maschine.** Die Entwicklungsumgebung dieses Agenten
ist Linux ohne GPU. C++/Vulkan/Qt-Code wird hier plattformneutral gebaut und
gegen Lavapipe (Software-Vulkan) funktional getestet; **Latenzmessungen sind nur
auf Ihrer RTX 5070 gültig**. Ab M1 liefere ich ein Skript
`tools/run_benchmarks.ps1`, das Sie lokal ausführen; die JSON-Ergebnisse
gehen in den Bericht. Alternativ: ein selbst gehosteter Windows-Runner
(siehe Rückfrage F3).

---

## 6. Magnetbasis (M1-Abnahme) – Zielskript

```python
from anvil import *
base = cylinder(d=25*mm, h=3*mm).chamfer(top=0.6*mm)
pocket = cylinder(d=5.1*mm, h=2.2*mm)        # 5×2-mm-Magnet, +0,1 mm Spiel
part = base - pocket
show(part)
export_stl(part, "base_25mm.stl", unit=mm, chord=0.01*mm)
```

Abnahme: dieselbe Sequenz über UI (Radialmenü → Cylinder, Eigenschaftsfeld
„25 mm“, Chamfer, zweiter Cylinder, Snap auf Zentrum, Boolean Difference,
Export) erzeugt im Journal **dieselben** Python-Befehle und eine bitgleiche
STL. Hinweis: `chamfer` auf einem Mesh-Primitiv ist nur für analytisch
bekannte Kanten definiert (Zylinderkante → Kegelstumpf); M1 implementiert
das parametrisch im Primitiv, M2 allgemein über OCCT.

---

## 7. Qualitätssicherung

- Unit (Catch2), Property-based (RapidCheck: z.B. „Boolean zweier gültiger
  Manifolds ist gültig“, „Export ∘ Import = Identität bis float32“),
  Golden Files (STL/3MF-Hashes), analytische Validierung (Kugel, Würfel, Torus:
  Volumen, Fläche, Konvergenzordnung 2 – Referenz bereits grün),
  Latenz-Regression (G1–G6) mit Schwelle p99 und Trend.
- CI lokal (Ihre Maschine oder self-hosted Runner); kein Cloud-Dienst sieht
  Projektdaten.

---

## 8. Datenschutz und Lokalität (ADR 0010)

Netzwerk standardmäßig aus, durchgesetzt auf drei Ebenen: (1) ANVIL öffnet
keine Sockets außer Loopback (Code-Audit + Laufzeit-Assertion im Socket-Layer),
(2) Installer legt ausgehende Firewall-Blockregeln für alle ANVIL-Binaries an,
(3) Sandbox-Prozess ohne `internetClient`-Capability (AppContainer).
Modell-Downloads: nur nach Dialog mit URL, Größe, Lizenz und gepinntem SHA-256;
Abweichung ⇒ Datei verworfen.

---

## 9. Rückfragen (5)

1. **Drucker:** Welche Modelle genau, Resin, FDM oder beides, und welcher Slicer
   (Chitubox, Lychee, PrusaSlicer, Bambu Studio, …)? Davon hängen Profile
   (Pixelgröße, Schichthöhe, Mindestwand, α_c) und das 3MF-Profil ab.
2. **Tablet:** Hersteller/Modell, und läuft es über Windows Ink oder Wintab?
   (Qt liefert Druck über `QTabletEvent`; Wintab braucht ggf. eigenen Pfad.)
3. **Monitor und Messumgebung:** Auflösung und Bildwiederholrate des
   Zielmonitors (das 16,7-ms-Ziel ist bei 4K deutlich härter als bei 1440p)?
   Und dürfen Benchmarks per Skript auf Ihrer Maschine laufen – manuell oder
   als selbst gehosteter Runner?
4. **Sculpt-Ziel:** Bestätigen Sie die Lesart „20 Mio. **Voxel** in aktiven
   Bricks“ (A3)? 20 Mio. Bricks à 8³ wären ≈ 10¹⁰ Voxel bzw. ≈ 20 GB allein
   für fp16-Distanzen.
5. **TRELLIS-Lizenzlage:** Der Code ist MIT, die Referenzpipeline hängt aber an
   nvdiffrast u.a. unter der NVIDIA Source Code License („research or
   evaluation“), und TRELLIS braucht laut Projekt ≥ 16 GB VRAM. Soll ich
   TRELLIS nur als Opt-in-Plugin mit Lizenzhinweis vorsehen (TripoSR, MIT, als
   Default), oder ganz weglassen?

### Antworten (2026-11-05)
1. Elegoo Mars 5 Ultra (Resin, 18 µm XY) und Elegoo Neptune (FDM, Modell offen); OrcaSlicer → `docs/DRUCKER.md`.
2. Tablet: spielt keine Rolle → Tablet-Druck entfällt (ADR 0015).
3. ASUS-Gaming-Monitor (Auflösung offen); Benchmarks per Skript `scripts/run-benchmarks.ps1`.
4. Lesart 20 Mio. Voxel bestätigt.
5. TRELLIS entfällt.
Zusatz: Upgradefähigkeit → ADR 0014.
