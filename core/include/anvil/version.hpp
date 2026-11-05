#pragma once
namespace anvil {
/// Application version (semantic versioning, see ADR 0014).
const char* version_string();
/// Version of the public Python API (`anvil.__api_version__`). Bumped on
/// incompatible changes; plugins declare the range they support.
inline constexpr int kApiVersion = 1;
/// Version of the .anvil document format (header line `# anvil-format: N`).
inline constexpr int kDocumentFormat = 1;
/// Version of settings.json.
inline constexpr int kSettingsFormat = 1;
}  // namespace anvil
