# Drittanbieter-Komponenten

Stand der Prüfung: 2026-11-05 (M1-Aktualisierung). Spalte „Status“: **verifiziert** = durch
Recherche mit Quelle belegt; **zu prüfen** = aus Vorwissen, wird beim Pinnen
in M1 (vcpkg-Baseline, Lizenzdatei im Paket) bestätigt. Versionen werden in
`vcpkg.json` (`overrides`) bzw. `requirements-*.txt` (`--require-hashes`)
festgeschrieben. Nutzung: privat, Einzelnutzer, Schweden.

## Kern (C++)

| Komponente | Version (Ziel) | Lizenz | Linkart | Status / Quelle |
|---|---|---|---|---|
| Qt 6 (Core, Gui, Quick, QuickControls2, ShaderTools) | neueste 6.x bei M1-Start; ≥ 6.10.2 | LGPL-3.0 (bzw. GPL-3.0) | dynamisch | 6.10.2 vom 29.01.2026 verifiziert ([Ankündigung](https://lists.qt-project.org/pipermail/announce/2026-January/000605.html)); neuere Patches zu prüfen |
| OpenCASCADE (OCCT) | 8.0.1 | LGPL-2.1 + OCCT-exception-1.0 | dynamisch (nur Worker) | 8.0.1 (30.07.2026) und Lizenz laut [Wikipedia](https://en.wikipedia.org/wiki/Open_Cascade_Technology), [OCCT Licensing](https://dev.opencascade.org/resources/licensing/); 8.0 verlangt ≥ C++17 |
| Manifold | 3.x (≥ 3.4; 3.5.2 upstream gemeldet) | Apache-2.0 | statisch | [Gentoo](https://packages.gentoo.org/packages/sci-mathematics/manifold), [openSUSE changelog](https://src.opensuse.org/pool/manifold/src/branch/factory/manifold.changes); exakte Version auf GitHub-Release zu prüfen |
| Embree | 4.4.x | Apache-2.0 | dynamisch/statisch | 4.4.0/4.4.1 verifiziert ([Release-Notes](https://newreleases.io/project/github/RenderKit/embree/release/v4.4.1)); Lizenz zu prüfen |
| OpenVDB (optional) | 13.0.x | Apache-2.0 (Umstellung von MPL-2.0) | dynamisch | 13.0.0 vom 03.11.2025 verifiziert ([Changes](https://www.openvdb.org/documentation/doxygen/changes.html)); Lizenzwechsel zu prüfen |
| oneTBB | 2022.x | Apache-2.0 | dynamisch | zu prüfen |
| pybind11 | 3.0.x | BSD-3-Clause | header-only | zu prüfen |
| CPython (embeddable) | 3.11.x (letzte Security-Version) | PSF-2.0 | dynamisch | Vorgabe; Security-Support bis 10/2027 |
| Vulkan-Headers / Loader / Validation Layers | SDK 1.4.x, API-Ziel 1.3 | Apache-2.0 | dynamisch (Loader) | zu prüfen |
| Vulkan Memory Allocator | 3.x | MIT | header-only | zu prüfen |
| volk | aktuell | MIT | statisch | zu prüfen |
| glslang | aktuell | BSD-3 / Apache-2.0 (gemischt) | Build-Werkzeug | zu prüfen |
| Eigen | 3.4.x / 5.x | MPL-2.0 | header-only | zu prüfen |
| AMGCL | aktuell | MIT | header-only | zu prüfen |
| lib3mf | 2.x | BSD-2-Clause | dynamisch | zu prüfen |
| tinyobjloader | 2.x | MIT | statisch | zu prüfen |
| stb_image | aktuell | MIT / Public Domain | statisch | zu prüfen |
| nlohmann/json, msgpack-c | aktuell | MIT / BSL-1.0 | header-only | zu prüfen |
| fmt, spdlog | aktuell | MIT | statisch | zu prüfen |
| Tracy (nur Dev) | 0.11+ | BSD-3-Clause | statisch | zu prüfen |
| Catch2, RapidCheck, Google Benchmark (nur Tests) | v3 / aktuell / 1.9+ | BSL-1.0 / BSD-2 / Apache-2.0 | – | zu prüfen |

## Tatsächlich in M1 eingebunden (gepinnt in `cmake/Versions.cmake`)

| Komponente | Version | Lizenz | Bezug |
|---|---|---|---|
| Manifold | v3.5.4 (Git-Tag, Commit `ce50d780`) | Apache-2.0 | FetchContent (zieht Clipper2 Commit `46f63917`, BSL-1.0) |
| pybind11 | v3.0.4 | BSD-3-Clause | FetchContent |
| Catch2 | v3.16.1 | BSL-1.0 | FetchContent (nur Tests) |
| Vulkan-Headers | vulkan-sdk-1.4.363.0 | Apache-2.0 / MIT | FetchContent, nur wenn kein SDK vorhanden |
| Qt | 6.8.3 (Windows-Ziel; Entwicklung Linux 6.4.2) | LGPL-3.0, dynamisch | aqtinstall 3.3.0 (MIT) aus download.qt.io, ohne Konto |
| CPython | 3.11 (aus lokaler Installation kopiert, ohne Netzwerkmodule) | PSF-2.0 | `scripts/build-windows.ps1` |
| Mesa Lavapipe | 25.2 | MIT | nur Linux-Tests (Software-Vulkan), nicht ausgeliefert |
| GitHub Actions (nur CI-Kompilierprüfung) | actions/checkout@v4, setup-python@v5, upload-artifact@v4, jurplel/install-qt-action@v4, ilammy/msvc-dev-cmd@v1 | MIT | manuell ausgelöst; sieht nur Quellcode |

Embree, OpenCASCADE, OpenVDB, oneTBB, Eigen, AMGCL, lib3mf, VMA sind in M1
**nicht** eingebunden (siehe ADR 0015); ihre Zeilen oben gelten ab dem
jeweiligen Meilenstein.

## KI-Worker (Python)

| Komponente | Version | Lizenz | Status / Quelle |
|---|---|---|---|
| PyTorch | ≥ 2.7 mit CUDA ≥ 12.8 (sm_120) | BSD-3-Clause | sm_120 in cu128-Builds laut [PyTorch-Forum](https://discuss.pytorch.org/t/nvidia-geforce-rtx-5070-ti-with-cuda-capability-sm-120/221509) |
| Keras | ≥ 3.12.3 (Sicherheitsfixes beim Laden) | Apache-2.0 | [3.12.3](https://newreleases.io/project/pypi/keras/release/3.12.3) verifiziert; neuere Releases vorhanden |
| TensorFlow (optional, WSL2) | ≥ 2.16 | Apache-2.0 | Windows-nativ ohne GPU seit 2.11 |
| ONNX Runtime | ≥ 1.20 | MIT | zu prüfen |
| numpy | 2.x | BSD-3-Clause | – |
| Jedi | aktuell | MIT | zu prüfen |
| Ollama (extern, nicht gebündelt) | Nutzerinstallation | MIT | Modelle: Lizenz je Modell, Prüfung beim Download-Dialog |

## Generative Modelle (Plugins, nicht gebündelt)

| Modell | Lizenz | EU | Entscheidung |
|---|---|---|---|
| TripoSR | MIT ([Modellkarte](https://huggingface.co/stabilityai/TripoSR)) | ja | Default |
| TRELLIS / TRELLIS.2 | Code MIT; nvdiffrast/nvdiffrec: NVIDIA Source Code License (research/evaluation) ([Issue #298](https://github.com/microsoft/TRELLIS/issues/298), [Comfy-Blog](https://blog.comfy.org/p/trellis2-and-pixal3d-are-now-native)) | unklar | **nicht eingebunden** (Nutzerentscheidung) |
| Hunyuan3D 2.0/2.1 | Tencent Community License, Territorium schließt EU, UK, Südkorea aus ([LICENSE](https://huggingface.co/tencent/Hunyuan3D-2/blob/main/LICENSE)) | **nein** | nicht eingebunden |

## Lizenzpflichten (Einzelnutzer)
LGPL-Komponenten (Qt, OCCT) werden dynamisch gelinkt und unverändert
ausgeliefert; Lizenztexte liegen im Installer unter `licenses/`. Apache-2.0-
`NOTICE`-Dateien werden mitkopiert. `tools/license-audit` (M7) vergleicht diese
Tabelle mit den tatsächlich installierten Paketen.
