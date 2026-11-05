# Entwicklung

## Linux (Agentenumgebung, Funktionstests)

```
sudo apt-get install qt6-base-dev qt6-declarative-dev qml6-module-qtquick{,-controls,-layouts,-window,-shapes,-templates,-dialogs} \
  qml6-module-qtqml-workerscript libvulkan-dev mesa-vulkan-drivers python3-dev xvfb glslang-tools
cmake --preset dev && cmake --build --preset dev
Xvfb :99 & export DISPLAY=:99      # Lavapipe = Software-Vulkan
ctest --preset dev                  # 31 Tests inkl. GUI-Selbsttest
```

## Struktur

| Ordner | Inhalt |
|---|---|
| `core/` | C++20 ohne Qt: Einheiten, Ausdrücke, Mesh-Prüfung, BVH, STL, Feature-DAG (Manifold), Dokument/Undo, Journal, Kamera, Snapping, Radial-Geste, Fuzzy-Suche |
| `python/src` | Session (Befehlsbus, eingebettetes CPython auf eigenem Thread), `_anvil`-Bridge, `anvil-cli` |
| `python/anvil` | öffentliche Python-API, Befehlsregister, Plugin-Lader |
| `app/` | Qt-Quick-App: Vulkan-Renderer (`src/vk`), Scene-Builder, Controller, QML-UI, Selbsttest |
| `app/render/shaders` | GLSL; `tools/compile_shaders.py` erzeugt das eingebettete SPIR-V (eingecheckt) |
| `tests/` | Catch2 (`unit/`), Python-Orakel (`reference/`), Abnahme (`acceptance/`) |
| `bench/` | Szenengenerator BS-5M, `anvil-bench`, Berichte |
| `scripts/` | Windows: Setup, Build/Paket, Benchmarks |

## Regeln
Siehe `CLAUDE.md`. Kurz: kein Netzwerk, keine Magic Numbers (Toleranzen in
`core/include/anvil/tolerance.hpp`), jede UI-Aktion als Python über
`AppController::execute`, Leistungsangaben nur mit Messung.
