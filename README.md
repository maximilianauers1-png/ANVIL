# ANVIL

Lokale 3D-CAD-Anwendung für hochdetaillierte Miniaturen und maßhaltige
Laborteile. 100 % offline, Vulkan-Viewport, Qt-Quick-Oberfläche, eingebettetes
Python, Hybrid-Kernel (OpenCASCADE · Manifold · SDF · Embree), lokale KI.

**Status:** M1 (Vertical Slice) fertig: Fenster, Vulkan-Viewport, Themes,
Radialmenü, Command Bar, eingebettetes Python, Einheiten, Undo, Primitive,
Manifold-Booleans, Snapping, geprüfter STL-Export, Autosave/Recovery.
Abnahme erfüllt: Magnetbasis per UI und per Skript → bitgleiche STL.
Start: [docs/handbuch/M1_Demo.md](docs/handbuch/M1_Demo.md) ·
Messwerte: [bench/reports/M1.md](bench/reports/M1.md)

| Dokument | Inhalt |
|---|---|
| [docs/M0_PLAN.md](docs/M0_PLAN.md) | Architektur, Annahmen, Meilensteine, Rückfragen |
| [docs/adr/](docs/adr/README.md) | Architekturentscheidungen |
| [docs/RISIKEN.md](docs/RISIKEN.md) | Risiken, Gate-Benchmarks |
| [THIRD_PARTY.md](THIRD_PARTY.md) | Abhängigkeiten, Versionen, Lizenzen |
| [bench/README.md](bench/README.md) | Benchmark-Szene BS-5M und Messprotokoll |
| [CLAUDE.md](CLAUDE.md) | Arbeitsregeln für Agenten |
| [docs/handbuch/M1_Demo.md](docs/handbuch/M1_Demo.md) | Bauen, Bedienung, Demo |
| [docs/DRUCKER.md](docs/DRUCKER.md) | Drucker- und Slicerdaten |
| [docs/ENTWICKLUNG.md](docs/ENTWICKLUNG.md) | Entwicklung, Struktur |

## Schnellstart

Windows: `scripts\setup-windows.ps1`, dann `scripts\build-windows.ps1` →
`dist\ANVIL\ANVIL.exe`. Linux (Entwicklung): siehe `docs/ENTWICKLUNG.md`.
