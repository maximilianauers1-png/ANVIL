# ADR 0005 – Eingebettetes Python und eine gemeinsame API

Status: vorgeschlagen

## Entscheidung
- Eingebettetes CPython 3.11 (offizielles Windows-Embeddable-Paket, im
  Installationsordner, isoliert von der System-Python-Installation per
  `PyConfig.isolated = 1` und `._pth`), Bindings via pybind11 3.x.
- Interpreter läuft auf einem **eigenen Thread**, nie auf UI- oder Render-Thread.
  Abbruch langer Skripte per `PyThreadState_SetAsyncExc(KeyboardInterrupt)`;
  C++-Kernaufrufe prüfen ein Abbruch-Token.
- Öffentliches Modul `anvil` (reines Python über `_anvil`): Primitive,
  Booleans (`-`, `|`, `&`), Transformationen, `show`, `export_*`, Einheiten
  (`mm`, `cm`, `m`, `inch`, `deg`), Variablen, Selektion, Befehle.
- **Eine API:** UI-Befehle sind registrierte Python-Funktionen mit
  Signatur-Metadaten (Typen, Einheiten, Defaults, Beschreibung). Aus denselben
  Metadaten entstehen (a) Eigenschaftspanel, (b) Fuzzy-Index der Command Bar,
  (c) JSON-Schema der KI-Tools, (d) Radialmenü-Einträge. Kein Befehl existiert
  nur in einer der drei Welten.
- Geometrieobjekte in Python sind *Handles auf DAG-Knoten*, keine Kopien.
  `a - b` erzeugt einen Boolean-Knoten (lazy); Auswertung bei `show`/Export.
- Startzeit: Python-Init ohne `numpy`-Import im Hauptprozess-Kritischpfad
  (Abschätzung ≈ 30–60 ms; Messung in G3). `numpy` wird beim ersten Bedarf
  geladen.
- Autocomplete: Jedi (MIT) im Python-Thread, Antworten asynchron.
