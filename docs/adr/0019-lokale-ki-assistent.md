# ADR 0019 – Lokaler KI-Assistent (Qwen über Ollama) und Verzerrungsfreies Posen

Status: angenommen · Datum: 2026-11-06 · Präzisiert ADR 0009/0010 (M6 vorgezogen, Lab/Print zurückgestellt)

## Kontext

Der Nutzer möchte eine lokale KI (Qwen, „eventuell PointNet“, Keras), die seine Netze bearbeitet,
mit einfachem Modell-Installer. Gleichzeitig verzerren importierte Tripo-Minis beim Posen
(Schulterplatten werden zu Spitzen gezogen).

## Entscheidungen

1. **Anwendungsfall:** ein Assistent, der natürliche Sprache in ANVIL-Werkzeugaufrufe übersetzt
   (Pose, Einzelgelenke, Höhe, Waffe, Sockel, Magnetloch, flacher Boden, Verschieben, Drehen,
   Verschmelzen, Verformungsoptionen, Ausblenden – 12 typisierte Werkzeuge mit JSON-Schema).
   Das Sprachmodell erzeugt **keinen Code**: jeder Aufruf wird geprüft (Objekt existiert und hat
   die passende Art, Aufzählungen, Wertebereiche) und von ANVIL in dokumentierte API-Aufrufe
   übersetzt. Der Nutzer sieht Zusammenfassung und Python und bestätigt; dann **ein** Befehl,
   **ein** Undo-Schritt, im Journal. Damit entfällt die Sandbox für KI-Code (ADR 0010 5.): es
   gibt keinen.
2. **Modell:** Qwen2.5 7B Instruct (Q4_K_M, 4,7 GB, Apache-2.0) als Standard – zuverlässiges
   Tool-Calling, Deutsch/Englisch, passt ins VRAM-Budget (ADR 0009: 9–10 GB für KI). Qwen3 8B
   und Qwen2.5 3B wählbar.
3. **Prozesse und Netz:** Das eingebettete Python hat kein `_socket` (Paket ohne Netzmodule).
   Nur `anvil-ai` (eigener C++-Code, ~300 Zeilen, keine Netzwerkbibliothek) öffnet Sockets und
   **nur zu Loopback-Adressen** (127.0.0.0/8, ::1; `OLLAMA_HOST`/`ANVIL_OLLAMA_URL` mit
   anderer Adresse ⇒ Ablehnung mit Meldung). Protokoll App↔Worker: JSON-Zeilen.
4. **Installer:** Ollama wird vom Nutzer einmal selbst installiert (ANVIL startet es nicht,
   M0-Annahme A8). Der Installer in ANVIL zeigt Modell, Größe, Lizenz und Quelle; „Install“
   lässt **Ollama** das Modell laden (`/api/pull`, Fortschritt gestreamt). Abweichung von
   ADR 0010 4.: kein in ANVIL gepinnter SHA-256 – die Modell-Digests konnten offline nicht
   ermittelt werden; Ollama prüft die SHA-256 jeder Schicht selbst (inhaltsadressiert,
   „verifying sha256 digest“).
5. **PointNet/Keras:** zurückgestellt. Ein gelernter Netz-Operator (z. B. Teilesegmentierung
   oder Skin-Gewichte) braucht Trainingsdaten (geriggte Charaktere mit Teilen), die nicht
   vorliegen; ohne sie wäre das Ergebnis schlechter als die geometrischen Verfahren unten.
6. **Verzerrung beim Posen** (geometrisch, kein ML):
   - getrennte kleine Schalen (≤ 0,25 · Höhe Bounding-Box-Diagonale: Schulterplatten,
     Platten, Taschen, Kopf/Hand als eigene Schale) folgen starr dem nächstgelegenen Knochen;
   - geodätische Voxel-Gewichte (Dionne & de Lasa 2013): Abstand durch das robust voxelisierte
     Volumen (h = H/160), Dijkstra je Knochen, w = exp(−(d − d_min)/σ), σ = 0,02 H, Top-4,
     zwei Glättungsschritte – für das Auto-Rig und wahlweise für das Skelett der Datei;
   - `rest='file'`: Pose 0 = Pose der Datei (keine Absenkung der T-Pose-Arme);
   - Messgröße `pose_distortion()`: Kantenlängenverhältnis posiert/Ruhe (max, p99, Anteil > 1,5).

## Tests

`ai_worker` (Worker gegen einen Ollama-Ersatzserver: Status, gestreamter Download, Fehler,
Chat, Löschen, Ablehnung von Nicht-Loopback), `ai_assistant` (Werkzeugschema, Abbildung,
8 ungültige Aufrufe abgewiesen), GUI-Selbsttest (KI-Modus, Installieren mit Fortschritt,
Frage, Vorschlag, Anwenden = 1 Befehl, Verwerfen). Mit echtem Qwen nicht in der
Linux-Agentenumgebung testbar (kein Zugriff auf die Modell-Registry) – Prüfung auf dem Zielsystem.
