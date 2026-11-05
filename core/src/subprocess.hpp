// Minimal child process with binary stdin/stdout pipes (Windows + POSIX).
// Reads happen on a dedicated thread so callers can wait with a timeout.
#pragma once
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace anvil {

class Subprocess {
  public:
    explicit Subprocess(const std::filesystem::path& exe);
    ~Subprocess();
    Subprocess(const Subprocess&) = delete;
    Subprocess& operator=(const Subprocess&) = delete;

    void write(const std::string& bytes);  // throws on a broken pipe
    /// Reads exactly n bytes; returns false on EOF (child died) or timeout.
    bool read(std::size_t n, std::string& out, std::chrono::milliseconds timeout, bool* timed_out);
    void kill();
    bool alive();

  private:
    void reader();
#ifdef _WIN32
    void* process_ = nullptr;
    void* in_w_ = nullptr;   // our end of the child's stdin
    void* out_r_ = nullptr;  // our end of the child's stdout
#else
    int pid_ = -1;
    int in_w_ = -1, out_r_ = -1;
#endif
    std::thread reader_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::string buf_;
    bool eof_ = false;
};

}  // namespace anvil
