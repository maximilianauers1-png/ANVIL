# ADR 0012 – Generative 3D-Modelle: Auswahl nach Lizenz (Schweden/EU)

Status: vorgeschlagen · offene Rückfrage F5

Keine Rechtsberatung; Bewertung anhand der Lizenztexte bzw. sekundärer Quellen,
Primärtext wird vor jeder Integration erneut geprüft und im Plugin-Manifest
verlinkt.

| Modell | Lizenz (Stand Recherche) | EU/Schweden | VRAM | Entscheidung |
|---|---|---|---|---|
| TripoSR (Stability AI + Tripo) | MIT (Modellkarte); Trainingsdaten Objaverse-Teilmenge (CC-BY) | erlaubt | ≈ 6 GB (Abschätzung) | **Default-Plugin** Bild → Mesh |
| TRELLIS / TRELLIS.2 (Microsoft) | Code MIT; Referenzpipeline nutzt nvdiffrast/nvdiffrec unter NVIDIA Source Code License (nur „research or evaluation“); Gewichte-Status strittig (GitHub-Issue #298) | privat-nichtkommerziell vermutlich vertretbar, aber Zweckbindung unklar | laut Projekt ≥ 16 GB | **Opt-in**, mit Lizenzhinweis, nur falls F5 zustimmt; bevorzugt Variante ohne nvdiffrast-Abhängigkeit |
| Hunyuan3D 2.0 / 2.1 (Tencent) | Community License: „Territory … excluding the European Union, United Kingdom and South Korea“ | **ausgeschlossen** | – | **nicht eingebunden** |
| Text → Bild (Konzeptbild) | je Modell prüfen (z. B. SDXL: OpenRAIL++-M; FLUX.1-schnell: Apache-2.0) | je Lizenz | 8–12 GB | Plugin-Schnittstelle, Auswahl in M6 |

Alle KI-Meshes durchlaufen zwingend: GWN-Reparatur → Remesh (DC) →
Print-Analyse → Exporter-Prüfung.
