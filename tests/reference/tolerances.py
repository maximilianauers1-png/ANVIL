"""Zentrale Toleranzen der Referenzimplementierung (Spiegel von core/include/anvil/tolerance.hpp, M1).

Alle Längen in mm (interne Einheit). Keine Toleranz darf außerhalb dieser Datei
als Literal auftauchen; siehe docs/adr/0006-einheiten-numerik-toleranzen.md.
"""
from __future__ import annotations

import math

# --- Ebene 0: reine Gleitkommaarithmetik ---------------------------------
# Relative Toleranz für Vergleiche von Größen, die durch wenige (< ~10)
# Rundungen entstanden sind: 16 ulp bei double.
REL_NUMERIC: float = 16.0 * 2.0**-52  # ≈ 3.55e-15
# Absolute Untergrenze für Vergleiche nahe 0 (mm). 1 pm: weit unter jeder
# physikalisch relevanten Länge, weit über double-Rauschen bei |x| ≤ 1e3 mm.
ABS_NUMERIC_MM: float = 1e-9

# --- Ebene 1: Geometrie-Kernel ---------------------------------------------
# Punktkoinzidenz im Modell (entspricht OCCT Precision::Confusion = 1e-7 in
# Modelleinheiten mm).
ABS_CONFUSION_MM: float = 1e-7
# Ein Dreieck gilt als degeneriert, wenn seine Fläche kleiner ist als
# DEGENERATE_AREA_REL · (Bounding-Box-Diagonale)². Skalierungsinvariant.
DEGENERATE_AREA_REL: float = 1e-14
# Ein Dreieck gilt außerdem als Nadel, wenn der minimale Innenwinkel unter
# dieser Schranke liegt (rad). Nur Warnung, kein Exportfehler.
NEEDLE_ANGLE_RAD: float = math.radians(0.01)

# --- Ebene 2: Fertigung -----------------------------------------------------
# Default-Sehnenfehler für Tesselierung in Exports, falls der Nutzer nichts
# vorgibt (mm). Resin-XY-Pixel liegen bei ~20–50 µm, also δ ≪ Pixel.
DEFAULT_CHORD_MM: float = 0.01  # M1: equal to the export default (ADR 0006)


def close(a: float, b: float, rel: float = REL_NUMERIC, abs_: float = ABS_NUMERIC_MM) -> bool:
    """|a−b| ≤ max(abs, rel·max(|a|,|b|)) — symmetrisch, im Gegensatz zu math.isclose-Defaults."""
    return abs(a - b) <= max(abs_, rel * max(abs(a), abs(b)))

# --- Ebene 3: Sculpting (M3, ADR 0008) ----------------------------------------
# Voxelgröße der SDF-Bricks: Standard 50 µm, Untergrenze 10 µm; Obergrenze der
# Voxel in der Bounding-Box eines Objekts.
DEFAULT_VOXEL_MM: float = 0.05
MIN_VOXEL_MM: float = 0.01
MAX_SCULPT_GRID_VOXELS: float = 1e10

# --- Ebene 4: Import (M4, ADR 0018) ---------------------------------------------
# Fast Winding Number: Cluster als Dipol, wenn Abstand > beta * Clusterradius.
WINDING_BETA: float = 2.0
# Standard-Zielhöhe importierter Minis (mm).
DEFAULT_MINI_HEIGHT_MM: float = 32.0
