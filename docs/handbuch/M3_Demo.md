# ANVIL M3 – Kurzanleitung: Modellieren wie mit Ton (Sculpting)

## 1. Losmodellieren in drei Klicks

1. Oben **Sculpt** wählen.
2. Ein Objekt anklicken → im Sculpt-Feld links **„Make … sculptable“**
   (oder Radial, Richtung SW). Das Objekt wird zu „Ton“ mit 0,05 mm Voxeln;
   große Objekte bekommen automatisch gröbere Voxel. Die ursprüngliche Form
   bleibt als parametrische Quelle im Baum erhalten. Die Umwandlung dauert je
   nach Größe einige Sekunden.
3. **Auf dem Objekt ziehen = modellieren, daneben ziehen = Ansicht drehen.**

| Pinsel | Wirkung | Kurzweg |
|---|---|---|
| Draw | Material aufbauen | Radial O |
| Carve | Material abtragen | Radial NO · **Strg** kehrt Draw/Carve um |
| Smooth | Oberfläche glätten (rundet Kanten, Flächen bleiben) | Radial N · **Umschalt** halten |
| Grab | Oberfläche mit dem Zeiger mitziehen | Radial NW |
| Flatten | auf die Ebene des ersten Berührpunkts abflachen | Radial W |

Radius (0,2–20 mm) und Stärke im Sculpt-Feld; der Kreis am Zeiger zeigt den
Radius auf der Oberfläche.

![Sculpt-Modus](img/m3_sculpt.jpg)

## 2. Ein Strich = ein Befehl

Jeder Strich steht als **ein** Eintrag in der Zeitleiste, z. B.
`sculpt("Kopf", "draw", [(…), (…)], radius=2*mm, strength=0.5)`, und ist ein
Undo-Schritt. Gespeicherte Dateien enthalten die Striche; das Laden ergibt
bitgenau dasselbe Modell.

## 3. Kombinieren und drucken

Ton-Objekte lassen sich mit allem anderen kombinieren, z. B. eine exakte
Bohrung oder ein Magnetloch abziehen (Design-Modus, „Cut with…“). Der Export
prüft wie immer das Netz; modellierte Flächen sind geschlossen und
schnittfrei. Scharfe Kanten des Ausgangskörpers werden beim Umwandeln um
höchstens eine Voxelbreite gebrochen; exakte Kanten setzt man danach mit
CAD-Körpern.

## 4. Skript

```python
show(sphere(d=20*mm), name="Kopf")
sculptable("Kopf", voxel=0.1*mm)
sculpt("Kopf", "draw", [(-3, 0, 10), (0, 1, 10), (3, 0, 10)], radius=3*mm, strength=0.8)
sculpt("Kopf", "smooth", [(-3, 0, 10), (3, 0, 10)], radius=3*mm, strength=1.0)
subtract("Kopf", cylinder(d=5.1*mm, h=2.2*mm).translate(z=-10*mm))
export_stl("Kopf", "kopf.stl")
```
