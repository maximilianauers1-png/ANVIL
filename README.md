# ANVIL

Lokale 3D-CAD-Anwendung für hochdetaillierte Miniaturen und maßhaltige
Laborteile. 100 % offline, Vulkan-Viewport, Qt-Quick-Oberfläche, eingebettetes
Python, Hybrid-Kernel (OpenCASCADE · Manifold · SDF · Embree), lokale KI.

**Status:** M0 (Planung) abgeschlossen, wartet auf Freigabe.

| Dokument | Inhalt |
|---|---|
| [docs/M0_PLAN.md](docs/M0_PLAN.md) | Architektur, Annahmen, Meilensteine, Rückfragen |
| [docs/adr/](docs/adr/README.md) | Architekturentscheidungen |
| [docs/RISIKEN.md](docs/RISIKEN.md) | Risiken, Gate-Benchmarks |
| [THIRD_PARTY.md](THIRD_PARTY.md) | Abhängigkeiten, Versionen, Lizenzen |
| [bench/README.md](bench/README.md) | Benchmark-Szene BS-5M und Messprotokoll |
| [CLAUDE.md](CLAUDE.md) | Arbeitsregeln für Agenten |

## Schnelltest (M0)

```bash
python -m unittest discover -s tests/reference -v
python bench/scene/generate_scene.py --out bench/out
```

Voraussetzung: Python ≥ 3.11 mit numpy.
