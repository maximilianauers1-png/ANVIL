#include "anvil/fsutil.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace anvil {

void flush_to_disk(std::FILE* f) {
    std::fflush(f);
#ifdef _WIN32
    _commit(_fileno(f));  // FlushFileBuffers on the underlying handle
#else
    ::fsync(fileno(f));
#endif
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open '" + p.string() + "'");
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

namespace {
std::FILE* open_file(const std::filesystem::path& p, const char* mode) {
#ifdef _WIN32
    std::wstring wm(mode, mode + std::char_traits<char>::length(mode));
    return _wfopen(p.c_str(), wm.c_str());
#else
    return std::fopen(p.c_str(), mode);
#endif
}
}  // namespace

void write_file_atomic(const std::filesystem::path& p, const std::string& data) {
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    auto tmp = p;
    tmp += ".tmp";
    std::FILE* f = open_file(tmp, "wb");
    if (!f) throw std::runtime_error("cannot write '" + tmp.string() + "'");
    std::size_t w = std::fwrite(data.data(), 1, data.size(), f);
    flush_to_disk(f);
    std::fclose(f);
    if (w != data.size()) throw std::runtime_error("short write to '" + tmp.string() + "' (disk full?)");
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);  // replaces existing target on POSIX and Windows (MoveFileEx)
    if (ec) throw std::runtime_error("cannot rename '" + tmp.string() + "': " + ec.message());
}

DurableAppender::DurableAppender(const std::filesystem::path& p) : path_(p) {
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    f_ = open_file(p, "ab");
    if (!f_) throw std::runtime_error("cannot open journal '" + p.string() + "'");
}
DurableAppender::~DurableAppender() {
    if (f_) std::fclose(f_);
}
void DurableAppender::append(const std::string& s) {
    if (std::fwrite(s.data(), 1, s.size(), f_) != s.size())
        throw std::runtime_error("journal write failed (disk full?)");
}
void DurableAppender::sync() { flush_to_disk(f_); }

}  // namespace anvil
