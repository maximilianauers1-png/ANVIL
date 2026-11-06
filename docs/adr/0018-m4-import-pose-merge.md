# ADR 0018 – M4 neu definiert: importierte Minis posen, ausrüsten, verschmelzen

Status: angenommen · Datum: 2026-11-06 · Ersetzt Teile von M0_PLAN §3.8 (Mini Forge)

## Kontext

Der M0-Plan sah für M4 einen Figurengenerator aus Armaturen vor (umgesetzt:
`figure()`, ADR 0017-Folgearbeit). Der Nutzer hat M4 neu ausgerichtet: Minis
in Tripo-/Meshy-Qualität lassen sich lokal nicht generieren; stattdessen
sollen **dort erzeugte Modelle importiert, leicht umgepost, mit Waffen
(Schwert, Zweihänder, Buster-Schwert …) ausgerüstet und mit weiteren Assets
zu einem druckbaren Objekt verschmolzen** werden.

KI-Netze sind für CAD-Booleans ungeeignet: Löcher, sich durchdringende
Teilschalen, nicht verschweißte UV-Nähte, gelegentlich verdrehte
Orientierung; nach dem Posen kommen Hautfalten hinzu (an gebeugten Gelenken
faltet sich die Oberfläche in sich selbst). Manifold (M1) lehnt solche Netze
ab.

## Entscheidungen

1. **Formate:** glTF 2.0 (GLB und .gltf mit Daten-URI/Begleitdatei) als
   Hauptformat – Tripo und Meshy exportieren es inkl. Rig. Dazu OBJ und STL.
   **FBX wird nicht unterstützt** (proprietär, kein lokales freies SDK ohne
   neue Abhängigkeit; Tripo/Meshy/Blender exportieren GLB). **Draco/Meshopt**
   werden mit präziser Meldung abgelehnt. Keine neue Abhängigkeit
   (nlohmann/json war vorhanden). Entfernte URIs werden verweigert (100 % lokal).
2. **Rahmen:** glTF ist Y-oben, Meter, Front +Z → ANVIL (x, y, z) ↦
   (x, −z, y)·1000 mm (Z oben, Front −Y). Gelenkrahmen werden konjugiert
   (C·W·C⁻¹) und orthonormalisiert. OBJ wird als Y-oben gelesen (Tripo/Meshy/
   Blender-Standard). Das Modell wird auf die Zielhöhe skaliert (Standard
   32 mm = `tol::kDefaultMiniHeightMm`), steht auf z = 0, ist in X/Y zentriert.
3. **Rig:** Aus dem Skin der Datei; Rollen aus Knochennamen (Mixamo, Unreal,
   Rigify, CamelCase-L/R, `_l`/`_r`), Seite notfalls aus der Position.
   Ohne Skin (oder ohne erkennbare Arme/Beine) **automatisches Rig**:
   17 Gelenke aus Körperproportionen + Querschnittsschwerpunkten; Gewichte
   nach Abstand zum Knochen mit Sichtbarkeitstest im Inneren (verallgemeinerte
   Windungszahl am Mittelpunkt) und Abfall exp(−(d − d_min)/σ), σ = 1,2 % der
   Höhe (≈ Gliedradius / 3).
4. **Pose-Parameter = die der parametrischen Figuren** (`l_elbow`,
   `r_shoulder_fwd`, `spine` …), daher gelten die Presets (stand, walk,
   attack, cast, guard) auch für Importe. Die Arme werden normiert: Winkel 0 =
   hängend, 10° abgespreizt, gestreckt – unabhängig davon, ob die Datei in
   T- oder A-Pose vorliegt (Ausrichtung per kürzestem Bogen auf −Z). Füße
   bleiben waagerecht; nach dem Posen steht das Modell wieder auf z = 0.
5. **Skinning: Dual Quaternions** (Kavan et al. 2008). Lineares Blending
   kollabiert gebeugte Gelenke („candy wrapper“): Unit-Test – Ellbogen 90°,
   Röhre r = 2 mm: DQS hält ≥ 1,6 mm Abstand zum Gelenk (LBS ≈ 1,41 mm).
6. **Robuste Voxelisierung** (`sdf::from_mesh_robust`):
   - Löcher werden durch Kegel über jede Randkomponente geschlossen
     (`cap_boundaries`): Der Rand eines Randes ist leer, also ist die
     Windungszahl danach außerhalb der Fläche ganzzahlig.
   - Vorzeichen = |w| > ½ der verallgemeinerten Windungszahl (Jacobson 2013),
     schnell per Dipol-Baum (Barill 2018, β = 2 = `tol::kWindingBeta`;
     Unit-Test: Abweichung zum exakten Wert < 0,05).
   - w ist in jeder dreiecksfreien Kugel konstant: Nachbarvoxel übernehmen
     das Vorzeichen, wenn der Abstand > h ist (Brick + 1-Voxel-Halo, strikt
     > h·(1+10⁻⁶)); w wird pro Brick nur ein- bis dreimal ausgewertet.
     Gitterpunkte genau auf der Fläche (Fußsohle z = 0, Kopf auf
     Gitterebene) werden 1 % eines Voxels versetzt getestet.
   - Beträge = Abstand nur zu **Randdreiecken** (w vor/hinter dem Dreieck an
     7 Stellen verschieden). Verdeckte Schalen und Hautfalten ziehen sonst |f|
     unter der echten Oberfläche gegen 0 und lassen Konturpunkte kollabieren.
     Überspringen von Bricks und Vorzeichenweitergabe nutzen weiterhin den
     Abstand zu allen Dreiecken (untere Schranke, 1-Lipschitz bleibt gültig).
   - Aufräumen bis zum Fixpunkt (`clean_for_print`): Hohlräume füllen
     (eingeschlossenes Harz), Krümel ≤ 27 Voxel entfernen, Zellen mit zwei
     Flächenblättern (Spalte < 1 Voxel, nicht druckbar) zur Seite des
     Festkörpers schließen; danach jede echte Vorzeichenkante auf |f| ≤ h
     begrenzen, damit die Kontur sie nie überspringt.
7. **Booleans:** Ist ein Operand nicht geschlossen-mannigfaltig, läuft die
   Boolesche Operation als Ton (SDF) – das Ergebnis ist immer geschlossen.
   Exakte CAD-Booleans (Bohrung in Ton) bleiben unverändert auf Manifold.
8. **Waffen** als SDF: Klingen/Köpfe = konvexe Hüllen (Max der Ebenenabstände),
   Griffe = Rundkegel; Teile überlappen (keine sich nur berührenden Flächen).
   Kanten ≥ 1,2 h. **Attach** setzt ein Objekt in die gepostete Hand
   (Faustmitte 70 % vom Handgelenk zu den Knöcheln, Achse nach vorn im
   kanonischen Unterarmrahmen) und folgt jeder Posenänderung (Referenz).
9. **Merge** = Vereinigung von Referenzen auf die Teile: alles bleibt
   parametrisch (Pose ändern → Schwert und Merge folgen); Teile werden
   ausgeblendet, nicht gelöscht.

## Benchmark (Linux, 4 vCPU Xeon 2,1 GHz; kein Beleg für das Zielsystem)

`bench/m4_bench.py`, Rohdaten `bench/reports/M4-linux-import.json`:

| Netz | Rig | Import | Pose | Waffe | Merge + Basis | Prüfung | ok |
|---|---|---|---|---|---|---|---|
| 2,9 k Dreiecke | Datei | 2 ms | 0,5 ms | 0,29 s | 8,6 s | 5,6 s | ja |
| 2,9 k | automatisch | 12 ms | 1 ms | 0,21 s | 9,7 s | 4,2 s | ja |
| 105 k | Datei | 0,06 s | 5 ms | 0,21 s | 12,6 s | 4,1 s | ja |
| 105 k | automatisch | 0,32 s | 4 ms | 0,19 s | 17,7 s | 5,8 s | ja |
| 419 k | Datei | 0,23 s | 20 ms | 0,21 s | 20,2 s | 3,7 s | ja |
| 419 k | automatisch | 1,40 s | 19 ms | 0,22 s | 26,6 s | 6,0 s | ja |

Posen ist interaktiv (≤ 20 ms bei 419 k Dreiecken); das Verschmelzen ist ein
einmaliger Schritt vor dem Export.

## Folgen / offene Punkte

- Das automatische Rig erwartet eine stehende Figur in T-/A-Pose oder mit
  hängenden Armen, Blick −Y. Statuen in dynamischer Pose (oder mit Waffe in
  der Hand) sollten in Tripo/Meshy geriggt werden. Verschiebbare Gelenke
  folgen bei Bedarf.
- Finger, Kiefer, Haare werden nicht einzeln gepost (folgen dem Elternknochen).
- Merge-Ergebnis bei 50 µm: ≈ 2,2 M Dreiecke (≈ 110 MB Binär-STL). Dezimierung
  vor dem Export gehört zu M5 (Druckvorbereitung).
- Merge-Zeit (9–27 s auf 4 vCPU) skaliert mit Kernzahl; auf dem Zielsystem
  mit `scripts/run-benchmarks.ps1` messen.
