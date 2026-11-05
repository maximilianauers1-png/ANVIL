# ADR 0015 – Abweichungen und Präzisierungen in M1 (mit Messwerten)

Status: umgesetzt (M1)

| # | Plan (M0) | M1-Umsetzung | Begründung / Messung | Rückkehr |
|---|---|---|---|---|
| 1 | Embree für BVH/Raycasts | eigene LBVH (Morton-Codes, Radix-Sort) in `core/bvh` | M1 braucht nur Picking, Snapping, Selbstschnitt. Gemessen (BS-5M, 4 vCPU Xeon 2,1 GHz): Aufbau 1,31 s, Raycast 1,4 µs, Snap-Abfrage p50 6 µs / p99 2,9 ms. Eine Abhängigkeit weniger, die auf dem Ziel-Build nicht getestet wäre. | Embree in M3 für Wandstärke, GWN, Sculpt-Picking; Vergleichsbenchmark dann |
| 2 | vcpkg-Manifest | `find_package` zuerst (vcpkg/System), sonst `FetchContent` mit gepinnten Git-Tags (`cmake/Versions.cmake`) | Ein Pfad für Linux-Agent und Windows; vcpkg bleibt nutzbar (Toolchain-Datei übergeben). Qt kommt als offizielle LGPL-Binärdistribution (ADR 0013). | – |
| 3 | Eigenes `VkDevice`, an Qt übergeben | Qt erzeugt das Gerät; ANVIL nutzt es über `QSGRendererInterface` | Ausreichend für M1 (Vulkan 1.0/1.1-Features, klassischer Render-Pass statt Dynamic Rendering). Eigenes Gerät nötig, sobald Mesh-Shader/Async-Compute kommen (M3). | M3 |
| 4 | QML AOT (`qmlcachegen`) | AOT ab Qt 6.6; auf Qt 6.4 (nur Linux-Entwicklung) interpretiert | Qt 6.4 `qmlcachegen` erzeugt bei Singleton-Lookups eine Endlosschleife (reproduziert, Stack in `AOTCompiledContext::loadSingletonLookup`). Ziel-Build nutzt Qt 6.8 mit AOT. | – |
| 5 | „Jede UI-Aktion erzeugt Python“ | Alle Befehle, Parameteränderungen, Ansichten (ViewCube, Ansicht-Menü), Einheit und Theme laufen als Python über den Befehlsbus. **Ausgenommen:** kontinuierliche Kamera-Navigation (Orbit/Pan/Zoom pro Mausbewegung) und Selektion per Klick | Sonst entstünden Hunderte Journal-Einträge pro Sekunde. Beides ist skriptbar (`view(...)`, `select(...)`) und ändert das Dokument nicht. | – |
| 6 | Befehl ohne Dokumentänderung = Undo-Schritt | Nur Befehle, die das Dokument ändern, erzeugen einen Undo-Schritt; alle Befehle stehen im Journal | Ein Undo, das nichts rückgängig macht (z. B. nach `export_stl`), wäre irreführend. | – |
| 7 | Tablet-Druck beim Sculpten | entfällt | Nutzerangabe: Tablet spielt keine Rolle. | – |
| 8 | TRELLIS optional | entfällt | Nutzerentscheidung (Lizenz nvdiffrast, ≥ 16 GB VRAM). | – |
| 9 | Default-Sehnenfehler 5 µm | 10 µm für Viewport **und** Export | Gleicher Wert → Export nutzt den Cache des Viewports; 10 µm ≪ 18 µm XY-Pixel des Mars 5 Ultra. | Einstellbar (`chord_mm`) |
| 10 | Kugel/Torus mit Sehnenfehler δ je Richtung | δ/2 je Richtung | Fehler gefunden durch Test: auf doppelt gekrümmten Flächen ist die Abweichung in der Facettenmitte ≈ Pfeilhöhe_u + Pfeilhöhe_v. Mit δ/2 wird max. Abweichung ≤ δ (Test prüft jede Facette). | – |
| 11 | Python im Hauptprozess mit vollem stdlib | Ausgelieferte Python-Laufzeit **ohne** `_socket`, `_ssl`, `urllib`, `http`, `email`, `asyncio` | Harte Garantie für Anforderung 2.1: eingebetteter Code (inkl. Plugins) kann keine Netzwerkverbindung öffnen (CI-Smoke-Test prüft `import socket` → Fehler). | Bei Bedarf (M6, nur 127.0.0.1) im KI-Worker, nicht im Hauptprozess |
| 12 | Selbstschnitt-Test mit exakten Prädikaten | Toleranzbehaftete double-Prädikate (`kSelfIntersectRel = 1e-9`, relativ zur Kantenlänge) | Für Manifold-Ausgaben (garantiert schnittfrei) und Importe ausreichend; exakte Shewchuk-Prädikate sind für M5 (Reparatur) eingeplant. | M5 |
