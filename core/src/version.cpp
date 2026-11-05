#include "anvil/version.hpp"
#ifndef ANVIL_VERSION_STRING
#error "ANVIL_VERSION_STRING must be defined by the build system"
#endif
namespace anvil {
const char* version_string() { return ANVIL_VERSION_STRING; }
}  // namespace anvil
