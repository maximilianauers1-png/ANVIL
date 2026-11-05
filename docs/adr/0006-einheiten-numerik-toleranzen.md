# ADR 0006 – Einheiten, Numerik, Toleranzen

Status: vorgeschlagen · Referenz: `tests/reference/tolerances.py`

## Entscheidung
- Intern SI-abgeleitet mit mm als Längeneinheit: double; kg, s, K, A, rad.
- C++: `template<Dim D> struct Quantity { double v; }`, `Dim` = rationale
  Exponenten (L, M, T, Θ, I, Winkel) als `constexpr`-Struktur; Addition nur
  bei gleicher Dimension (Compile-Fehler), Multiplikation addiert Exponenten.
  Winkel als eigene Dimension, damit „mm + deg“ scheitert, aber `rad` per
  explizitem `.value()` dimensionslos nutzbar ist.
- Anzeigeeinheit (mm/cm/m/inch) ist reiner View-State; Umschalten ändert nie
  gespeicherte Werte. Anzeige mit signifikanten Stellen nach Feldtoleranz.
- Feldausdrücke: Grammatik `expr := term (('+'|'-') term)*`, Einheiten als
  postfix-Faktoren, Variablen, Funktionen (`sqrt`, `sin` mit Winkel-Dimension).
  Ergebnis wird **als Ausdruck** gespeichert (parametrisch), angezeigt als Wert.
- GPU: kamera-relative float-Positionen (ADR 0007).

## Toleranzen (drei Ebenen, alle zentral)
| Ebene | Name | Wert | Verwendung |
|---|---|---|---|
| Numerik | `REL_NUMERIC` | 16·2⁻⁵² ≈ 3,6·10⁻¹⁵ | Vergleich weniger Rundungen |
| Numerik | `ABS_NUMERIC_MM` | 10⁻⁹ mm | Untergrenze nahe 0 |
| Kernel | `ABS_CONFUSION_MM` | 10⁻⁷ mm | Punktkoinzidenz (= OCCT Confusion) |
| Kernel | `DEGENERATE_AREA_REL` | 10⁻¹⁴ · diag² | degenerierte Dreiecke |
| Fertigung | `DEFAULT_CHORD_MM` | 5 µm | Default-Sehnenfehler |
| Fertigung | Profilwerte | Nutzer | Mindestwand, Pixel, α_c |

Vergleich: |a−b| ≤ max(abs, rel·max(|a|,|b|)) (symmetrisch).

## Testfall 25,4 mm ↔ 1 inch
1 inch ≡ 25,4 mm exakt (Definition 1959). In double ist 25,4 nicht exakt
darstellbar, aber `x/25.4*25.4` weicht höchstens wenige ulp ab; Test über 10⁴
Zufallswerte in [−10⁴, 10⁴] mm mit `REL_NUMERIC` ist grün.
