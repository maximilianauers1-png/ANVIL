# ADR 0007 – Renderer: Vulkan 1.3, Frame-Budget, Präzision

Status: vorgeschlagen · Gate: G1

## Entscheidung
- Vulkan 1.3 Core (Dynamic Rendering, Synchronization2, Timeline-Semaphoren,
  Buffer Device Address, Descriptor Indexing). Speicher über VMA.
  Shader GLSL → SPIR-V offline (glslang), eingebettet; Pipeline-Cache auf Platte.
- Geometrie: GPU-residente Meshes, Positionen fp32 objekt-lokal relativ zum
  Objektschwerpunkt, Normalen oktaeder-kodiert (2×16 bit). Optional Meshlets
  (64 V / 124 T) mit Mesh-Shader-Culling (Frustum + Normalenkegel).
- Präzision: Modellmatrix in double; pro Frame M' = T(−c)·M in double, dann
  float. Bildfehler ≈ ‖x − c‖·2⁻²⁴; bei 1 m Abstand ≈ 60 nm.
- Tiefenpuffer: Reversed-Z, unendliche Fernebene, D32_SFLOAT.
- Darstellung: Matcap/PBR-light, Kanten via Screen-Space-Normalen/ID-Kanten
  (keine Geometrie-Duplikation), optional SSAO halbe Auflösung.
- Frame-Pacing: FIFO-Present (vsync), 2 Frames in flight; `present_wait`
  für Messung.

## Frame-Budget (Ziel 16,7 ms, p99) – Planwerte, durch G1 zu belegen
| Posten | Budget |
|---|---|
| Hauptpass 5 M Dreiecke | ≤ 4,0 ms |
| ID-/Kanten-/Selektionspass | ≤ 1,5 ms |
| SSAO + Post | ≤ 2,0 ms |
| Qt Quick UI | ≤ 1,5 ms |
| Overlays (Snaps, Gizmos, ViewCube) | ≤ 0,5 ms |
| Reserve (Compositor, Treiber, Jitter) | ≥ 7,2 ms |

CPU-Seite: Render-Thread ≤ 3 ms pro Frame (Kommandoaufzeichnung,
Uniform-Updates); keine Allokationen im Frame-Loop (Tracy-Messung).
