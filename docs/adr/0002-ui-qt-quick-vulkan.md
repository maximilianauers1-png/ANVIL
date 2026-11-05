# ADR 0002 – UI: Qt 6 Quick über gemeinsamem Vulkan-Gerät

Status: vorgeschlagen · Gates: G1, G2, G4 (M1)

## Kontext
Vorgabe: Qt 6 Quick (LGPL, dynamisch) über dem Viewport; Wechsel zu eigener
GPU-UI bei Latenz-/Integrationsproblemen. Viewport p99 ≤ 16,7 ms bei 5 M
Dreiecken; Radialmenü ≤ 1 Frame; Render-Thread blockiert nie.

## Entscheidung
1. ANVIL erzeugt `VkInstance` (Vulkan 1.3) und `VkDevice` selbst, mit allen
   benötigten Features/Extensions, und übergibt sie Qt per
   `QQuickWindow::setGraphicsDevice(QQuickGraphicsDevice::fromDeviceObjects(...))`.
   Damit gibt es genau ein Gerät, keine Ressourcen-Teilung über Gerätegrenzen.
2. Qt Quick nutzt den **threaded render loop**; der Viewport zeichnet im Signal
   `beforeRenderPassRecording` mit `beginExternalCommands()` direkt in Qts
   Render-Pass (Underlay). Keine Zwischentextur, keine Zusatzlatenz.
3. Schwere GPU-Arbeit (Sculpt-Remeshing, Analysen, Picking-Rebuild) läuft auf
   einer eigenen Compute-Queue, synchronisiert über Timeline-Semaphoren; der
   Render-Thread nimmt nur fertige Ergebnisse (Pufferwechsel per Atomic).
4. QML wird AOT-kompiliert (`qmlcachegen`/`qt_add_qml_module`); das Radialmenü
   ist beim Start instanziiert und nur unsichtbar.
5. Qt-Version: aktuellste 6.x-Patchversion bei M1-Start (Stand Recherche: 6.10.2
   vom 29.01.2026 bestätigt; neuere Versionen auf dem Build-System prüfen).
   Mindestens 6.8 (LTS), da `QQuickRhiItem` (6.7+) als Fallback-Pfad dient.

## Fallback-Kaskade (nur per Messung)
a) Underlay (Default) → b) `QQuickRhiItem` (Textur, +1 Blit ≈ 0,1 ms bei
1440p) → c) Viewport im eigenen Swapchain-Fenster, Qt Quick als
`QQuickRenderControl` in Offscreen-Textur, Komposition durch ANVIL →
d) eigene GPU-UI (ImGui-artig, immediate mode). Jeder Wechsel erfordert ein
ADR-Update mit Messwerten.

## Messung
G1: BS-5M, Kamerapfade aus `bench/README.md`, Frametime aus
`VK_KHR_present_id`/`present_wait`-Zeitstempeln + GPU-Timestamp-Queries;
Kriterium p99 ≤ 16,7 ms über 3 × 1800 Frames.
G2: Synthetische Taste per `SendInput`, Zeitstempel des Ereignisses vs.
Present des ersten Frames, der das Menü enthält (Frame-Tag über
Pixel-Readback einer Markerzeile). Kriterium: Menü im ersten Frame, dessen
Aufzeichnung nach dem Ereignis beginnt.
G4: Qt-RHI und eigener Renderer teilen Gerät ohne Validierungsfehler
(Validation Layers + Synchronization Validation im Debug-Build).
