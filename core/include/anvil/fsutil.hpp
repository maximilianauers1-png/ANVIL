// File helpers: atomic writes and durable appends (journal, autosave, exports).
#pragma once
#include <cstdio>
#include <filesystem>
#include <string>

namespace anvil {

std::string read_file(const std::filesystem::path& p);
/// Writes to "<p>.tmp", flushes to disk, then renames over `p` (atomic on NTFS/POSIX).
void write_file_atomic(const std::filesystem::path& p, const std::string& data);

/// Append-only file whose `sync()` guarantees durability (fsync / FlushFileBuffers).
class DurableAppender {
  public:
    explicit DurableAppender(const std::filesystem::path& p);
    ~DurableAppender();
    DurableAppender(const DurableAppender&) = delete;
    DurableAppender& operator=(const DurableAppender&) = delete;
    void append(const std::string& s);
    void sync();
    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
    std::FILE* f_ = nullptr;
};

/// fsync for an open FILE*.
void flush_to_disk(std::FILE* f);

}  // namespace anvil
