# ADR 0001 – Prozessmodell und Crash-Isolation

Status: vorgeschlagen (M0) · Gate: G5 (M2)

## Kontext
Anforderung 2.6: kein Arbeitsverlust beim Absturz eines Subsystems. Anforderung
3: KI-Worker als separater Prozess. Bekannte Absturzquellen: OCCT auf
pathologischen B-Reps (Fillets, Booleans, STEP-Import – Segfaults und
Endlosschleifen sind dokumentiert), Treiberfehler in CUDA, Python-C-Erweiterungen,
fremder Code aus KI-Ausgaben.

## Entscheidung
Vier Prozesse, Kommunikation über Named Pipes (Steuerung, Nachrichten mit
Längenpräfix, MessagePack) und benannte Shared-Memory-Sektionen (Massendaten:
Meshes, Bilder, Tensoren; Ringpuffer mit Generationszähler):

| Prozess | Inhalt | Begründung |
|---|---|---|
| `anvil.exe` | UI, Renderer, Dokument, Journal, Manifold, Embree, SDF, eingebettetes Python | latenzkritisch; Manifold garantiert mannigfaltige Ergebnisse und ist robust auf gültigen Eingaben |
| `anvil-geom.exe` | OCCT, STEP/IGES, Import-Reparatur, FEM | absturz-/hängeanfällig auf fremden Eingaben |
| `anvil-ai` | CPython + Keras/Torch/ONNX, Ollama-Client | großer, treiberabhängiger Stack; Importzeit 2–5 s darf Kaltstart nicht treffen |
| `anvil-sandbox.exe` | Ausführung von KI-generiertem Python | Sicherheitsgrenze (ADR 0010) |

Worker werden von einem Supervisor im Hauptprozess gestartet, mit Heartbeat
(250 ms) überwacht, per Job Object an die Lebensdauer des Hauptprozesses
gebunden und bei Absturz/Timeout neu gestartet. Der laufende Job wird als
fehlgeschlagen mit Ursache (Exit-Code, Minidump-Pfad) gemeldet; das Dokument
bleibt unverändert, da Worker nur Ergebnisse liefern, nie das Dokument mutieren.

Absicherung des Hauptprozesses selbst: Journal vor Ausführung persistiert
(ADR 0004); unbehandelte Ausnahmen schreiben Minidump; Neustart bietet
Recovery an.

## Alternativen
- Alles in einem Prozess: niedrigste Latenz, verletzt 2.6.
- Manifold ebenfalls auslagern: +IPC-Kopie je Boolean. Abschätzung: 1 M
  Dreiecke ≈ 36 MB, memcpy in Shared Memory ≈ 5–10 ms – akzeptabel für
  Einzeloperationen, nicht für Live-Vorschau beim Ziehen. Verworfen; Option
  bleibt per Konfiguration erhalten, falls G5 Instabilität zeigt.

## Messung (Gate G5)
Fault-Injection: Worker per `TerminateProcess`, Zugriffsverletzung und
Endlosschleife während Viewport-Interaktion abschießen. Kriterium: kein
Frame > 16,7 ms durch den Absturz, Dokument unverändert, Neustart < 1 s.
