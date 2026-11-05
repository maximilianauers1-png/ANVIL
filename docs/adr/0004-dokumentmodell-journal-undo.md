# ADR 0004 – Dokumentmodell: Feature-DAG, Transaktionsjournal, Undo

Status: vorgeschlagen

## Entscheidung
- **Feature-DAG.** Knoten = (Typ, Parameter, Eingänge). Parameter sind
  `Quantity`-Werte oder Ausdrücke über benannte Variablen. Ergebnis-Cache
  inhaltsadressiert: key = SHA-256(Typ ‖ kanonische Parameter ‖ Eingangs-Keys ‖
  Kernel-Version). Änderung einer Variablen invalidiert genau die transitive
  Hülle ihrer Nutzer; Neuberechnung topologisch sortiert, unabhängige Zweige
  parallel (TBB Flow Graph), Geometrie-Worker-Aufrufe asynchron.
- **Command.** Jede Mutation ist ein `Command` mit (a) kanonischer
  Python-Quelle, (b) strukturierter Form (JSON), (c) Inverse oder Snapshot-Ref.
  UI-Aktionen erzeugen *nur* Commands; es gibt keinen zweiten Mutationspfad.
- **Transaktion = Undo-Schritt.** Ein Python-Befehl der Command Bar, ein
  Skriptlauf oder ein angenommener KI-Vorschlag bilden je eine Transaktion.
  Kontinuierliche Interaktionen (Ziehen, Pinselstrich) werden zu *einem*
  Command verschmolzen (Start bei Press, Commit bei Release).
- **Journal.** Append-only `journal.log`: Datensatz = {seq, Zeit, Transaktion,
  CRC32C}. Schreiben vor Ausführung (write-ahead), `FlushFileBuffers`.
  Abschätzung: NVMe-Flush ≈ 0,1–1 ms, außerhalb des Render-Threads.
  Pinselstriche speichern Stroke-Samples (Position, Druck, Zeit) statt
  Voxeldaten → Replay deterministisch und kompakt.
- **Autosave.** Snapshot alle 120 s oder 200 Transaktionen (konfigurierbar) in
  `autosave/` mit atomarem Rename; Journal danach rotiert.
- **Recovery.** Beim Start: unvollständige Journal-Endsätze (CRC) verwerfen,
  Snapshot laden, Rest replayen, Nutzer fragen.
- **Python ist serialisierbar.** Das Journal ist ein gültiges Python-Skript
  (Kommentare tragen Metadaten): `anvil --replay journal.log` reproduziert das
  Dokument.

## Konsequenzen
Undo = Zurücksetzen auf Zustand vor Transaktion: bei invertierbaren Commands
per Inverse, sonst per Cache/Snapshot-Referenz. Sculpt-Undo hält pro Strich
geänderte Bricks (Copy-on-Write), Speicher begrenzt (Default 2 GB RAM).
