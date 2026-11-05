# Single place for pinned third-party versions (see THIRD_PARTY.md, ADR 0014).
# Upgrading a dependency = change one line here, rebuild, run ctest + benchmarks.
set(ANVIL_MANIFOLD_TAG   v3.5.4)   # Apache-2.0
set(ANVIL_PYBIND11_TAG   v3.0.4)   # BSD-3-Clause
set(ANVIL_EIGEN_TAG     3.4.0)    # MPL-2.0 (header-only)
set(ANVIL_OCCT_TAG      V8_0_1)   # LGPL-2.1 + exception, only in anvil-geom
set(ANVIL_JSON_TAG      v3.11.3)  # MIT (header-only)
set(ANVIL_CATCH2_TAG     v3.16.1)  # BSL-1.0
set(ANVIL_VKHEADERS_TAG  vulkan-sdk-1.4.363.0)  # Apache-2.0 (only if no system headers)
set(ANVIL_QT_MIN         6.4)      # Linux dev minimum; Windows target >= 6.8
set(ANVIL_PYTHON_VERSION 3.11)     # target interpreter on Windows (any 3.11+ works for dev)
