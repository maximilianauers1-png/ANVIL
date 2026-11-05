"""Binäres STL (Referenz). Größe exakt 84 + 50·N Byte.

Layout: 80 Byte Header, uint32 N (little endian), dann N × {float32 n[3],
float32 v0[3], v1[3], v2[3], uint16 attr}. STL ist einheitenlos; ANVIL schreibt
die Einheit in den Header ("ANVIL unit=mm ...") und zeigt sie im Exportdialog.
"""
from __future__ import annotations

import hashlib
from pathlib import Path

import numpy as np

STL_DTYPE = np.dtype([("n", "<f4", (3,)), ("v", "<f4", (3, 3)), ("attr", "<u2")])
assert STL_DTYPE.itemsize == 50


def to_records(V: np.ndarray, F: np.ndarray) -> np.ndarray:
    tri = V[F]  # m×3×3, float64
    n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    ln = np.linalg.norm(n, axis=1, keepdims=True)
    n = np.divide(n, ln, out=np.zeros_like(n), where=ln > 0)
    rec = np.zeros(len(F), dtype=STL_DTYPE)
    rec["n"] = n
    rec["v"] = tri
    return rec


def header(unit: str = "mm", note: str = "") -> bytes:
    h = f"ANVIL binary STL unit={unit} {note}".encode("ascii")[:80]
    if h.lower().startswith(b"solid"):  # würde von Lesern als ASCII missdeutet
        raise ValueError("binary STL header must not start with 'solid'")
    return h.ljust(80, b" ")


def write_binary(path: Path, records: np.ndarray, unit: str = "mm", note: str = "") -> str:
    """Schreibt STL und liefert SHA-256 (hex) des Dateiinhalts."""
    h = hashlib.sha256()
    with open(path, "wb") as f:
        for chunk in (header(unit, note), np.uint32(len(records)).tobytes(), records.tobytes()):
            f.write(chunk)
            h.update(chunk)
    return h.hexdigest()


def read_binary(path: Path) -> tuple[bytes, np.ndarray]:
    data = Path(path).read_bytes()
    n = int(np.frombuffer(data, "<u4", 1, 80)[0])
    if len(data) != 84 + 50 * n:
        raise ValueError(f"size {len(data)} ≠ 84 + 50·{n}")
    return data[:80], np.frombuffer(data, STL_DTYPE, n, 84)
