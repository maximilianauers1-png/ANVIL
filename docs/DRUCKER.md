# Drucker und Slicer (Nutzerangaben, M1)

| Gerät | Verfahren | Kennwerte | Quelle | Status |
|---|---|---|---|---|
| Elegoo Mars 5 Ultra | Resin (MSLA) | 9K-Mono-LCD 8520 × 4320, **XY 18 × 18 µm**, Bauraum 153,36 × 77,76 × 165 mm, Schicht 0,01–0,2 mm | [Elegoo Wiki](https://wiki.elegoo.com/mars/spec), [3DJake](https://3djake.com/elegoo/mars-5-ultra) | verifiziert (eine Quelle nennt abweichend 12K/16–19 µm – verworfen) |
| Elegoo Neptune (Modell offen) | FDM | Düse, Bauraum, Mindestwand je Modell | – | **Rückfrage: welches Neptune-Modell?** |
| OrcaSlicer | Slicer (FDM; 3MF/STL) | Import STL in mm; 3MF-Projekt ab M5 | – | – |

Folgen für ANVIL:
- Default-Sehnenfehler 10 µm < halbe Pixelgröße des Mars 5 Ultra (18 µm).
- Voxelgröße für Minis (M3): h ≤ 0,05 mm ≈ 2,8 Pixel; kleinste darstellbare
  Struktur ≈ 2h = 0,1 mm, also gut 5 Pixel → Detail-Linter-Grenze (M4) wird
  aus dem Profil abgeleitet, nicht geraten.
- Resin-Profile (M5): Kritikwinkel, Mindestwand und Material sind
  Nutzereingaben (keine geratenen Materialwerte, Anforderung 11).
- STL-Export schreibt mm; OrcaSlicer interpretiert STL als mm. Der
  Exportdialog zeigt die Einheit groß an.
