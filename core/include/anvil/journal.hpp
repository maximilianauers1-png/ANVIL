// Write-ahead transaction journal for crash recovery (ADR 0004).
//
// The journal is itself a readable Python file. Each record:
//   #@txn <seq> <origin> crc=<crc32c hex> bytes=<n>
//   <python source, n bytes>
//   #@end <seq>
// written and flushed to disk BEFORE the command executes; after success
//   #@ok <seq>   (or #@fail <seq> if it raised and was rolled back)
// is appended. Undo/redo are recorded as "#@undo" / "#@redo". A clean
// shutdown appends "#@closed". Recovery replays committed transactions and
// reports the last unfinished one (it may be what crashed the process).
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "anvil/fsutil.hpp"

namespace anvil {

std::uint32_t crc32c(const std::string& s);

struct JournalAction {
    enum Kind { Txn, Undo, Redo } kind = Txn;
    std::uint64_t seq = 0;
    std::string origin;
    std::string source;
    bool ok = false;      // Txn: completed successfully
    bool failed = false;  // Txn: raised an error and was rolled back (not a crash)
};

struct JournalContents {
    std::vector<JournalAction> actions;
    bool clean_close = false;
    bool truncated_tail = false;  // last record incomplete or CRC mismatch (crash while writing)
    std::string format_header;
};

class Journal {
  public:
    explicit Journal(const std::filesystem::path& path);
    /// Appends the transaction and syncs it to disk. Returns its sequence number.
    std::uint64_t begin_txn(const std::string& origin, const std::string& source);
    void mark_ok(std::uint64_t seq);
    void mark_failed(std::uint64_t seq);
    void mark_undo();
    void mark_redo();
    void close_clean();
    const std::filesystem::path& path() const { return out_->path(); }

    static JournalContents read(const std::filesystem::path& path);

  private:
    std::unique_ptr<DurableAppender> out_;
    std::uint64_t seq_ = 0;
};

}  // namespace anvil
