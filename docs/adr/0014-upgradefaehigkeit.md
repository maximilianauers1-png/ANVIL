# ADR 0014 – Upgradefähigkeit

Status: umgesetzt (M1) · Anlass: Nutzerwunsch „Das Programm soll upgradebar sein“

## Ziel
Neue ANVIL-Versionen, neue Abhängigkeiten und eigene Erweiterungen sollen
eingespielt werden können, ohne Dokumente, Einstellungen oder Plugins zu
brechen und ohne Netzwerkzugriff zur Laufzeit.

## Entscheidung (fünf versionierte Schnittstellen)

| Schnittstelle | Version | Ort | Regel |
|---|---|---|---|
| Anwendung | SemVer `MAJOR.MINOR.PATCH` | `VERSION` | MAJOR bei inkompatiblen Datei-/API-Änderungen |
| Dokumentformat `.anvil` | `# anvil-format: N` (Zeile 1) | `kDocumentFormat` | Ältere Formate werden beim Öffnen migriert; neuere werden mit präziser Meldung abgelehnt (`Session::open`, `anvil.load`) |
| Python-API | `anvil.__api_version__` | `kApiVersion` | Plugins deklarieren `"api": [min, max]`; inkompatible Plugins werden übersprungen, nicht geladen |
| Einstellungen | `"version"` in `settings.json` | `kSettingsFormat` | Migrationskette N→N+1 in `Settings::load`; Dateien neuerer Versionen werden nicht überschrieben |
| Journal | `# ANVIL journal v1` | `journal.cpp` | Recovery liest nur bekannte Versionen |

Weitere Bausteine:

- **Ein Ort für Abhängigkeitsversionen:** `cmake/Versions.cmake`. Upgrade =
  eine Zeile ändern → `ctest` + Benchmarks → Commit. Geometrie-Upgrades
  (Manifold) werden durch die bitgleichen Golden-Hashes der Abnahmetests
  sichtbar, nicht stillschweigend.
- **Dokumente sind Python-Skripte** über die öffentliche API: auch ohne
  Migration lesbar und von Hand reparierbar.
- **Benutzerdaten getrennt von der Installation:** Einstellungen
  (`%APPDATA%\ANVIL`), Sitzungen/Journale (`%LOCALAPPDATA%\ANVIL\sessions`),
  Plugins (`%APPDATA%\ANVIL\plugins`). Ein Update ersetzt nur `dist\ANVIL`.
- **Portable, side-by-side:** `dist\ANVIL` ist ein in sich geschlossener
  Ordner (Qt-DLLs, isoliertes Python). Mehrere Versionen können parallel
  liegen; Rückkehr zur alten Version = alten Ordner starten.
- **Offline-Update:** Ein Update ist ein neuer Ordner bzw. ein ZIP aus
  `scripts/build-windows.ps1` (lokal gebaut) – kein Update-Server, kein
  Hintergrunddienst. Integrität bei Weitergabe über SHA-256 der ZIP-Datei
  (ab M7 im Installer geprüft).
- **Plugins:** `plugin.json` + `plugin.py` mit `register(registry)`; Befehle
  erscheinen automatisch in Command Bar und Radial-Editor.

## Konsequenzen
Jede inkompatible Änderung braucht eine Migration plus Test (Golden-Datei des
alten Formats). M1 hat Format 1 überall, Migrationsketten sind vorbereitet,
aber noch leer.
