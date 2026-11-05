# CLAUDE.md – Arbeitsregeln für ANVIL

ANVIL ist eine lokale 3D-CAD-Desktop-Anwendung für einen einzigen Nutzer
(Festkörperphysiker; D&D-Minis und maßhaltige Laborteile). Ziel: Windows 11,
RTX 5070 (12 GB), 64 GB RAM, Python 3.11.

## Sprache
- UI-Texte, Bezeichner, Code-Kommentare zu APIs: **Englisch**.
- Dokumentation (`docs/`, ADRs, Berichte, Handbuch): **Deutsch**.

## Unverhandelbar (vor jeder Änderung prüfen)
1. 100 % lokal. Keine Netzwerkzugriffe außer 127.0.0.1. Keine Telemetrie.
   Neue Abhängigkeiten nur mit Eintrag in `THIRD_PARTY.md` (Version, Lizenz, Quelle).
2. Render-Thread blockiert nie. Keine Datei-I/O, Locks mit Wartezeit oder
   Allokationen im Frame-Loop.
3. Alles per Maus bedienbar; jede UI-Aktion erzeugt einen Python-`Command`.
4. Jeder Befehl = ein Undo-Schritt. Ein einziger Mutationspfad (Command Bus).
5. Kein STL-Export ohne bestandene Prüfungen; Fehler präzise benennen.
6. Journal wird vor Ausführung persistiert (Crash-Recovery).

## Numerik
- Intern double, mm. Typen `Quantity<Dim>` (C++) bzw. `Quantity` (Python).
- **Keine Magic Numbers.** Toleranzen nur aus `core/include/anvil/tolerance.hpp`
  (Spiegel: `tests/reference/tolerances.py`). UI-Konstanten nur aus Design Tokens.
- Formeln im Code mit Quelle/Herleitung im Kommentar.
- Unbekannte Materialwerte nie raten – vom Nutzer erfragen.

## Architektur
Abweichungen von `docs/M0_PLAN.md` / `docs/adr/` nur per neuem oder
aktualisiertem ADR **mit Benchmark**. Prozessgrenzen (ADR 0001) nicht
aufweichen: OCCT nur in `anvil-geom`, KI nur in `anvil-ai`, KI-Code nur in
`anvil-sandbox`.

## Tests und Benchmarks
- Python-Referenz: `python -m unittest discover -s tests/reference -v`
- Benchmark-Szene: `python bench/scene/generate_scene.py --out bench/out`
- C++ (ab M1): `cmake --preset dev && cmake --build --preset dev && ctest --preset dev`
- Leistungsangaben nur mit Messung (`bench/reports/`). Messungen aus der
  Linux-Agentenumgebung sind **keine** Belege für Zielsystem-Latenz.
- Ziel verfehlt ⇒ Messwert, Ursache, Alternative melden; Ziel nicht aufweichen.

## Meilensteine
Nach jedem Meilenstein: Tests grün, `bench/reports/Mx.md`, Commit, kurze
Demo-Anleitung. Nach M0 auf Freigabe warten (erledigt, sobald Nutzer freigibt).

## Git
Branch `claude/admiring-cannon-rd3ip8`. Keine Modellbezeichner in Commits oder Code.
Große Artefakte (`bench/out/`, Builds) nie committen.
