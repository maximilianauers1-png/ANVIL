# ADR 0009 – KI-Backend und VRAM-Manager

Status: vorgeschlagen

## Befund
- TensorFlow ≥ 2.11 unterstützt auf nativem Windows keine GPU; GPU nur über WSL2.
- PyTorch-Wheels mit CUDA 12.8+ enthalten sm_120 (Blackwell, RTX 50xx) laut
  PyTorch-Maintainern seit 2.7; Triton-Fehler („sm_120 is not defined“) treten
  bei veralteten Toolchains auf. → Beim Setup wird
  `torch.cuda.get_arch_list()` auf `sm_120` geprüft, sonst klare Fehlermeldung.
- Keras 3 ist backend-agnostisch; `.keras`-Dateien sind nur portabel, wenn das
  Modell `keras.ops` statt backend-spezifischer Ops nutzt. Keras 3.12.3+
  enthält Härtungen für Laden/Deserialisierung; wir laden trotzdem nur mit
  `safe_mode=True` und nur im KI-Worker.

## Entscheidung
- `KERAS_BACKEND=torch`, PyTorch cu128 (oder neuer), eigene venv des
  KI-Workers mit gepinnten Hashes (`pip --require-hashes`).
- ONNX Runtime (CUDA/TensorRT-EP) als Inferenzalternative; Export via
  `torch.onnx` bzw. Keras-Export.
- TensorFlow optional über WSL2-Worker (gleiche IPC über `\\wsl$`-unabhängigen
  Loopback-TCP auf 127.0.0.1 – einzige Ausnahme von Named Pipes, nur weil WSL2
  keine Windows-Named-Pipes teilt).

## VRAM-Manager
Budget B = VRAM_total − Reserve_OS(0,6 GB) − Reserve_Viewport. Viewport-Reserve
dynamisch = max(1,5 GB, 1,3 × aktueller Viewport-Bedarf). Modelle melden ihren
Bedarf vor dem Laden; Verdrängung nach LRU; höchstens ein „großes“ Modell
(> 4 GB) gleichzeitig. Ollama-Modelle werden über `keep_alive=0` entladen.
Statusleiste: belegt / Budget / größter Verbraucher. Messung über
`VK_EXT_memory_budget` (Viewport) und NVML (Prozesse).

Abschätzung 12 GB: 0,6 OS + 1,5–2,5 Viewport ⇒ 9–10 GB für KI. Ein 7–8B-LLM
in Q4 (≈ 5 GB) passt; TRELLIS (laut Projekt ≥ 16 GB) passt **nicht** (ADR 0012).
