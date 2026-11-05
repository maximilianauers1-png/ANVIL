#include "anvil/journal.hpp"

#include <array>
#include <cstdio>
#include <sstream>

#include "anvil/version.hpp"

namespace anvil {

std::uint32_t crc32c(const std::string& s) {
    // CRC-32C (Castagnoli), reflected polynomial 0x82F63B78.
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0x82F63B78u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : s) c = table[(c ^ b) & 0xff] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

Journal::Journal(const std::filesystem::path& path) {
    bool fresh = !std::filesystem::exists(path) || std::filesystem::file_size(path) == 0;
    if (!fresh) {
        auto c = read(path);
        for (const auto& a : c.actions) seq_ = std::max(seq_, a.seq);
    }
    out_ = std::make_unique<DurableAppender>(path);
    if (fresh) {
        out_->append("# ANVIL journal v1 (anvil " + std::string(version_string()) +
                     ") -- replay with: anvil-cli --replay <this file>\n");
        out_->sync();
    }
}

std::uint64_t Journal::begin_txn(const std::string& origin, const std::string& source) {
    const std::uint64_t seq = ++seq_;
    char hdr[128];
    std::snprintf(hdr, sizeof hdr, "#@txn %llu %s crc=%08x bytes=%zu\n", static_cast<unsigned long long>(seq),
                  origin.c_str(), crc32c(source), source.size());
    std::string rec = hdr + source;
    if (source.empty() || source.back() != '\n') rec += "\n";
    rec += "#@end " + std::to_string(seq) + "\n";
    out_->append(rec);
    out_->sync();  // durable before execution (write-ahead)
    return seq;
}

void Journal::mark_ok(std::uint64_t seq) {
    out_->append("#@ok " + std::to_string(seq) + "\n");
    out_->sync();
}
void Journal::mark_failed(std::uint64_t seq) {
    out_->append("#@fail " + std::to_string(seq) + "\n");
    out_->sync();
}
void Journal::mark_undo() {
    out_->append("#@undo\n");
    out_->sync();
}
void Journal::mark_redo() {
    out_->append("#@redo\n");
    out_->sync();
}
void Journal::close_clean() {
    out_->append("#@closed\n");
    out_->sync();
}

JournalContents Journal::read(const std::filesystem::path& path) {
    JournalContents c;
    std::string data = read_file(path);
    std::size_t pos = 0;
    auto next_line = [&](std::string& line) {
        if (pos >= data.size()) return false;
        std::size_t e = data.find('\n', pos);
        if (e == std::string::npos) {
            line = data.substr(pos);
            pos = data.size();
            return true;
        }
        line = data.substr(pos, e - pos);
        pos = e + 1;
        return true;
    };
    std::string line;
    if (next_line(line)) c.format_header = line;
    while (next_line(line)) {
        if (line.rfind("#@txn ", 0) == 0) {
            unsigned long long seq = 0;
            char origin[64] = {};
            unsigned crc = 0;
            std::size_t bytes = 0;
            if (std::sscanf(line.c_str(), "#@txn %llu %63s crc=%x bytes=%zu", &seq, origin, &crc, &bytes) != 4 ||
                pos + bytes > data.size()) {
                c.truncated_tail = true;
                break;
            }
            std::string src = data.substr(pos, bytes);
            pos += bytes;
            if (pos < data.size() && data[pos] == '\n' && (src.empty() || src.back() != '\n')) ++pos;
            std::string end;
            if (crc32c(src) != crc || !next_line(end) || end != "#@end " + std::to_string(seq)) {
                c.truncated_tail = true;
                break;
            }
            c.actions.push_back({JournalAction::Txn, seq, origin, src, false});
        } else if (line.rfind("#@ok ", 0) == 0) {
            std::uint64_t seq = std::stoull(line.substr(5));
            for (auto it = c.actions.rbegin(); it != c.actions.rend(); ++it)
                if (it->kind == JournalAction::Txn && it->seq == seq) {
                    it->ok = true;
                    break;
                }
        } else if (line.rfind("#@fail ", 0) == 0) {
            std::uint64_t seq = std::stoull(line.substr(7));
            for (auto it = c.actions.rbegin(); it != c.actions.rend(); ++it)
                if (it->kind == JournalAction::Txn && it->seq == seq) {
                    it->failed = true;
                    break;
                }
        } else if (line == "#@undo") {
            c.actions.push_back({JournalAction::Undo});
        } else if (line == "#@redo") {
            c.actions.push_back({JournalAction::Redo});
        } else if (line == "#@closed") {
            c.clean_close = true;
        }
    }
    return c;
}

}  // namespace anvil
