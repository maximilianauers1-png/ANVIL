# ADR 0010 – Lokalität, Netzwerk, Sandbox

Status: vorgeschlagen

## Entscheidung
1. **Kein Netzwerk per Default.** Alle Sockets laufen durch eine ANVIL-Schicht,
   die nur `127.0.0.1`/`::1` erlaubt (Assertion + Log). Keine Telemetrie, kein
   Update-Check, kein Konto. Qt-Netzwerkmodul wird nicht gelinkt, außer für
   den Ollama-Client im KI-Worker (Loopback).
2. **Firewall.** Der Installer legt ausgehende Blockregeln für alle
   ANVIL-Binaries an (`New-NetFirewallRule -Direction Outbound -Action Block`),
   Loopback ist davon unberührt.
3. **Ollama.** Erwartet unter `127.0.0.1:11434`. ANVIL prüft `OLLAMA_HOST`;
   ist er nicht Loopback, wird die Verbindung verweigert und gewarnt.
   Hinweis in der Doku: Ollama selbst kann Update-Checks durchführen – das
   liegt außerhalb von ANVIL und wird in der Anleitung zur Abschaltung bzw.
   Firewall-Regel beschrieben.
4. **Modell-Downloads** nur über einen expliziten Dialog (Quelle, Größe,
   Lizenz, SHA-256). Der Hash ist im Plugin-Manifest gepinnt; Abweichung ⇒
   Datei gelöscht, Fehler gemeldet. Danach Netzwerk wieder aus.
5. **Sandbox für KI-Code.** `anvil-sandbox.exe` startet als AppContainer-
   Prozess ohne Capabilities (kein `internetClient`, kein `privateNetwork`),
   in einem Job Object (Speicher-/CPU-Limit, kein Kindprozess); ACL-Grant nur
   auf den Projektordner. Der KI-Code läuft dort gegen einen Dokument-Proxy,
   der API-Aufrufe aufzeichnet. Ergebnis ist eine **Transaktion als Daten**
   (JSON) plus Vorschau-Mesh; der Hauptprozess führt keinen KI-Code aus,
   sondern wendet die validierte Transaktion an. Die Python-Darstellung wird
   dem Nutzer als Diff gezeigt.

## Test (M7)
Offline-Test: Netzwerkadapter deaktiviert, kompletter Funktionsdurchlauf;
zusätzlich Mitschnitt (pktmon) im Normalbetrieb: 0 Pakete außer Loopback.
