#include "paths.hpp"

#include <cstdlib>
#include <iterator>

#ifdef _WIN32
#include <windows.h>
#endif

namespace anvil {

std::filesystem::path executable_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::current_path() : p.parent_path();
#endif
}

std::filesystem::path default_python_dir() {
    if (const char* env = std::getenv("ANVIL_PYTHON_DIR")) return env;
    auto installed = executable_dir() / "python";
    if (std::filesystem::exists(installed / "anvil" / "__init__.py")) return installed;
    return ANVIL_SOURCE_PYTHON_DIR;
}

}  // namespace anvil
