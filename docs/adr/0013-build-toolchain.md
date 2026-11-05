# ADR 0013 – Build und Toolchain

Status: vorgeschlagen

- C++20, MSVC v143+ (Windows, Ziel), Clang 18+ (Linux-CI des Agenten,
  plattformneutraler Code), CMake ≥ 3.28 mit Presets, Ninja.
- Abhängigkeiten über vcpkg-Manifest (`vcpkg.json`) mit gepinntem
  `builtin-baseline` (Commit-Hash) und `overrides` für exakte Versionen;
  Binary-Caching lokal (`VCPKG_BINARY_SOURCES=clear;files,...`), kein Cloud-Cache.
- Qt **nicht** über vcpkg, sondern als offizielle LGPL-Binärdistribution
  (Online-Installer einmalig oder Offline-Paket), dynamisch gelinkt; Pfad
  über `CMAKE_PREFIX_PATH`. Grund: Build-Zeit (> 1 h) und LGPL-Nachweis
  (unveränderte, austauschbare DLLs).
- OCCT ebenfalls DLL (LGPL-2.1 + Exception); nur `anvil-geom.exe` lädt sie.
- Warnungen: `/W4 /permissive- /WX` für eigenen Code; ASan-Build (MSVC) für
  Leak-/Speicherfehler-Tests; Tracy für Profiling (nur Dev-Builds).
- Tests: Catch2 v3, RapidCheck, Google Benchmark; Python-Referenztests
  mit `unittest` (keine Zusatzabhängigkeit).
