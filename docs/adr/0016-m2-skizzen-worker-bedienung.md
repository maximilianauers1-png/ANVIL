# ADR 0016 – M2: Skizzen, B-Rep-Worker, gemischte Auswertung, Zeichenbedienung

Status: umgesetzt (M2) · Messwerte: `bench/reports/M2.md`

## Kontext

M2 bringt exakte Features (OCCT) und Skizzen. Der Nutzer verlangt dazu
ausdrücklich: Zeichnen (Polylinie, Solidify) und Abziehen (z. B.
Lithografiemasken) mit minimalem Aufwand, „Streamlining hat höchste Priorität“.

## Entscheidungen

| # | Entscheidung | Begründung / Messung |
|---|---|---|
| 1 | OCCT ausschließlich im Prozess `anvil-geom` (ADR 0001), Protokoll: Länge (u32 LE) + JSON-Kopf + binäres Netz über stdin/stdout; der Worker schreibt das Protokoll auf einen duplizierten Deskriptor, stdout → NUL | OCCT-Absturz trifft nur den Worker (Test `geom_debug_crash`); der STEP-Writer schreibt Meldungen auf stdout und hätte sonst das Protokoll zerstört (gefunden im Test). |
| 2 | Ein Baum, der B-Rep-Knoten enthält, wird im Worker ausgewertet; enthält ein Teilbaum Mesh-Knoten (Solidify, Deform, Import), werden nur die exakten Teilbäume im Worker berechnet und mit Manifold kombiniert | Masken-Workflow (verrundetes Teil − Leiterbahn) funktioniert; 7,9 ms. Ein exaktes Feature **auf** Mesh-Geometrie (z. B. Fillet nach Solidify) wird mit präziser Meldung abgelehnt statt still angenähert. |
| 3 | Skizzen-Solver: Minimum-Norm-Gauß-Newton (δq = −Jᵀ(JJᵀ+μI)⁻¹f, Armijo-Backtracking), LM (Marquardt-Skalierung) als Rückfall; DOF/Freiheit per SVD, Konflikte per Leave-one-out | LM allein scheiterte an lösbaren großen Skizzen (Benchmark). Minimum-Norm bewegt beim Ziehen nur, was die Bedingungen erzwingen. |
| 4 | Auswertung löst Skizzen ohne Analyse (`analyze=false`) | × 135 bei 400 Segmenten. |
| 5 | Zeichnen ohne Skizziermodus: die Ebene ergibt sich aus dem ersten Klick (achsparallele Fläche unter dem Cursor, sonst Boden); Fangen an Geometrie, H/V-Inferenz (8 px), adaptives Raster (≈ 12 px), Schließen am Startpunkt (10 px), getippte Länge | Ein Werkzeug = ein Befehl (`sketch_polyline/rect/circle`), danach ist die Skizze gewählt und die Schnellleiste bietet Extrude/Solidify/Cut/Revolve/Edit. Kein Moduswechsel, kein Dialog. |
| 6 | Koordinaten werden gerundet: freie Punkte auf 10^⌊log₁₀ wpp⌋ (≤ 1 Pixel), alle Werte auf `tol::kAbsNumericMm` | Ohne Rundung entstanden Befehle wie `offset=3.0000000000000284*mm`. Die Rundung liegt unter jeder geometrischen Toleranz. |
| 7 | „Cut through“ merkt sich das Teil, auf dem gezeichnet wurde; sonst ein Klick auf das Teil. „Cut with…“: Werkzeuge im Viewport anklicken, Enter | Lithografiemaske: Rechteck auf Fläche zeichnen → ein Klick. |
| 8 | Skizzeneditor als Overlay: Punkte ziehen (`sketch_move`), jede gemessene Länge anklicken und Wert tippen (`sketch_dim`), Bemaßungen ändern (`sketch_set`), Bedingungs-Leiste, Farbe nach Freiheitsgrad | Bemaßen ohne Werkzeugwahl: Klick auf die angezeigte Länge. |
| 9 | Parameter-Scrubbing: Parametername horizontal ziehen, Geist-Vorschau (`edit_preview`, ≤ 1 pro 50 ms), Loslassen = ein `edit(...)` = ein Undo-Schritt | Kontinuierliche Änderung ohne Journalflut (Anforderung 2.5). Längen logarithmisch (≈ 1 %/px), Winkel 0,5°/px, Faktoren 0,005/px, Umschalt = fein. |
| 10 | Linux-Entwicklung mit OCCT 7.6.3 (Distribution), Windows-Ziel 8.0.1 (Quellbau) | Gleiche API-Teilmenge; CI prüft den 8.0.1-Pfad. |

## Folgen

- `geometry.cpp` kennt `has_mesh_only()`; neue Mesh-Features müssen dort
  eingetragen werden.
- Der Radialring „sketch“ erscheint automatisch, wenn genau eine Skizze
  gewählt ist (Kontextauflösung im Controller).
