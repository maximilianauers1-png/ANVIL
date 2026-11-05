# ADR 0003 – Hybrid-Geometriekernel und Konverter

Status: vorgeschlagen

## Entscheidung
Kein eigener B-Rep-Kernel. Vier Repräsentationen mit expliziten, verlustbehafteten
Konvertern; jede Konvertierung ist ein sichtbarer DAG-Knoten mit Fehlerschranke.

| Repräsentation | Bibliothek | Zweck |
|---|---|---|
| B-Rep | OpenCASCADE (OCCT) 8.0.x, im Geometrie-Worker | Laborteile, STEP, Fillets, exakte Maße |
| Mesh (manifold) | Manifold 3.x | schnelle, robuste Booleans, Minis |
| SDF (Sparse Bricks) | eigen; OpenVDB optional für Import/Export | Sculpting, glatte Vereinigungen, Reparatur |
| BVH | Embree 4.4 | Picking, Snapping, Selbstschnitt, Wandstärke, Raumwinkel |

Konverter (mit Fehlerschranke als Parameter):
- B-Rep → Mesh: `BRepMesh_IncrementalMesh` mit Sehnenfehler δ und Winkeltoleranz;
  Ergebnis via `MeshGL` an Manifold; Prüfung durch Exporter-Validator.
- Mesh → SDF: Generalized Winding Number für das Vorzeichen, Embree für den
  Abstand, nur im Schmalband.
- SDF → Mesh: Manifold Dual Contouring (garantiert 2-mannigfaltig).
- Mesh → B-Rep: nur als facettiertes B-Rep (für STEP-Export von Mesh-Teilen),
  ausdrücklich als „facetted“ gekennzeichnet; keine Flächenrückführung.

Die Ziel-API (`cylinder(...) - cylinder(...)`) erzeugt in M1 Manifold-Knoten.
Ab M2 entscheidet der Knotentyp, welcher Kernel rechnet; gemischte Bäume
konvertieren an der Grenze explizit, nicht implizit.

## Determinismus
Bitgleiche Ergebnisse für UI vs. Skript (M1-Abnahme) verlangen deterministische
Kernel. Manifold parallelisiert mit TBB; Determinismus wird durch einen
Property-Test (100 Wiederholungen, Hash-Vergleich, 1 vs. N Threads) geprüft.
Schlägt er fehl: Single-Thread-Modus für Export-Builds oder kanonische
Sortierung des Ergebnisses vor dem Hash.
