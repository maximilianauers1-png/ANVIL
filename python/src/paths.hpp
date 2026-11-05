#pragma once
#include <filesystem>

namespace anvil {
/// Directory of the running executable.
std::filesystem::path executable_dir();
/// Directory containing the `anvil` Python package: $ANVIL_PYTHON_DIR, else
/// <exe>/python (installed layout), else the source tree (development).
std::filesystem::path default_python_dir();
}  // namespace anvil
