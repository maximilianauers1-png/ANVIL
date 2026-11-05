# Risikoregister und Gate-Benchmarks

Bewertung: Wahrscheinlichkeit (W) × Auswirkung (A), je 1–5.

| # | Risiko | W | A | Maßnahme / Gate | Alternative, falls Gate scheitert |
|---|---|---|---|---|---|
| R1 | Qt Quick + eigener Vulkan-Renderer im selben Pass erreicht p99 ≤ 16,7 ms nicht (Sync-Stalls, Qt-Scene-Graph-Kosten) | 2 | 5 | **G1/G4** in M1, Woche 1 – vor allen Features | Fallback-Kaskade ADR 0002 (RhiItem → RenderControl → eigene GPU-UI) |
| R2 | Kaltstart > 3 s (Qt-DLLs, NVIDIA-Treiberinit 0,2–0,5 s, QML, Python) | 3 | 3 | **G3**: Messung mit ETW/`QElapsedTimer` je Phase; Worker lazy; Pipeline-Cache; QML AOT | Splash mit funktionsfähigem Viewport ≠ Ziel; stattdessen: Treiberinit parallel zu Qt-Init, DLL-Delay-Load |
| R3 | Radialmenü > 1 Frame durch QML-Sync | 2 | 4 | **G2**; Menü vorinstanziiert | Menü als nativer Overlay-Pass im Viewport-Renderer |
| R4 | Manifold nicht deterministisch über Threads → UI/Skript nicht bitgleich | 2 | 3 | Property-Test ADR 0003 | Kanonische Ergebnis-Sortierung vor Hash; Abnahme auf geometrische Gleichheit (Hausdorff = 0) |
| R5 | OCCT-Abstürze/Hänger | 4 | 2 | Worker-Isolation (ADR 0001), Timeouts | – (bereits mitigiert) |
| R6 | PyTorch/Triton auf Windows + sm_120 instabil | 3 | 3 | Setup-Check `get_arch_list()`; ONNX Runtime als Inferenzpfad | Training in WSL2 |
| R7 | TRELLIS passt nicht in 12 GB VRAM; Lizenz unklar | 4 | 2 | TripoSR Default (ADR 0012) | TRELLIS weglassen |
| R8 | Sculpt-Ziel missverstanden (A3) | 3 | 4 | Rückfrage F4 | Out-of-Core-Streaming |
| R9 | Selbstschnitt-Test bei 5 M Dreiecken zu langsam für interaktiven Export | 3 | 2 | Embree-BVH, parallel; Abschätzung O(N log N), Ziel < 2 s bei 5 M | Inkrementell (nur geänderte Regionen, Cache je DAG-Knoten) |
| R10 | Agent-Umgebung ohne Windows/GPU: Latenzbelege nur auf Nutzermaschine | 5 | 3 | `tools/run_benchmarks.ps1` ab M1; Lavapipe für Funktionstests | Self-hosted Runner (F3) |
| R11 | Python 3.11 Security-EOL Okt. 2027 | 5 | 1 | Bewusst akzeptiert | Migration 3.12/3.13 per ADR |
| R12 | LGPL-Pflichten (Qt, OCCT) bei Weitergabe | 1 | 2 | Dynamisch linken, Lizenztexte beilegen | – (Einzelnutzer) |
| R13 | Umfang: M4 (Mini Forge) und M6 (Neural Lab) sind je eigenständige Produkte | 4 | 3 | Strikte Abnahmekriterien, keine Nebenfeatures | Priorisierung mit Nutzer nach M3 |

## Gate-Definitionen (gemessen auf Zielsystem, Release-Build)

| Gate | Messgröße | Kriterium | Methode |
|---|---|---|---|
| G1 | Frametime Viewport, BS-5M, 1440p | p99 ≤ 16,7 ms, 3 × 1800 Frames | `present_wait` + GPU-Timestamps |
| G2 | Taste → Menü sichtbar | im ersten Frame nach Ereignis | `SendInput`, Frame-Tag-Readback |
| G3 | Kaltstart → erster interaktiver Frame | < 3,0 s (Median von 10, nach Reboot-freiem Cache-Flush via RAMMap „Empty Standby List“) | Prozessstart-Zeitstempel bis erstes Present |
| G4 | Vulkan-Validierung | 0 Fehler inkl. Sync-Validation | Validation Layers |
| G5 | Worker-Absturz | 0 Frames > 16,7 ms, Dokument unverändert | Fault-Injection |
| G6 | Sculpt-Strich in BS-5M + 20 M Voxel aktiv | Viewport p99 ≤ 16,7 ms; Strich-zu-Sichtbar ≤ 2 Frames | Aufgezeichnete Tablet-Strokes, Replay |
