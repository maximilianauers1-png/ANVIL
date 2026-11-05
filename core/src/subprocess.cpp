#include "subprocess.hpp"

#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace anvil {

#ifdef _WIN32
Subprocess::Subprocess(const std::filesystem::path& exe) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE in_r, in_w, out_r, out_w;
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0))
        throw std::runtime_error("CreatePipe failed");
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);   // our ends are not inherited
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (!ok) {
        CloseHandle(in_w);
        CloseHandle(out_r);
        throw std::runtime_error("cannot start '" + exe.string() + "' (error " + std::to_string(GetLastError()) + ")");
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess, in_w_ = in_w, out_r_ = out_r;
    reader_ = std::thread([this] { reader(); });
}

Subprocess::~Subprocess() {
    kill();
    if (in_w_) CloseHandle(in_w_);
    if (reader_.joinable()) reader_.join();
    if (out_r_) CloseHandle(out_r_);
    if (process_) CloseHandle(process_);
}

void Subprocess::write(const std::string& b) {
    std::size_t off = 0;
    while (off < b.size()) {
        DWORD w = 0;
        if (!WriteFile(in_w_, b.data() + off, DWORD(b.size() - off), &w, nullptr))
            throw std::runtime_error("worker pipe closed");
        off += w;
    }
}

void Subprocess::reader() {
    char tmp[65536];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(out_r_, tmp, sizeof tmp, &n, nullptr) || n == 0) break;
        std::lock_guard lk(mu_);
        buf_.append(tmp, n);
        cv_.notify_all();
    }
    std::lock_guard lk(mu_);
    eof_ = true;
    cv_.notify_all();
}

void Subprocess::kill() {
    if (process_) TerminateProcess(process_, 1);
}

bool Subprocess::alive() { return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT; }

#else
Subprocess::Subprocess(const std::filesystem::path& exe) {
    int in[2], out[2];
    if (pipe(in) || pipe(out)) throw std::runtime_error("pipe failed");
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addclose(&fa, in[1]);
    posix_spawn_file_actions_addclose(&fa, out[0]);
    std::string path = exe.string();
    char* argv[] = {path.data(), nullptr};
    int rc = posix_spawn(&pid_, path.c_str(), &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]);
    close(out[1]);
    if (rc != 0) {
        close(in[1]);
        close(out[0]);
        throw std::runtime_error("cannot start '" + path + "' (errno " + std::to_string(rc) + ")");
    }
    fcntl(in[1], F_SETFD, FD_CLOEXEC);
    fcntl(out[0], F_SETFD, FD_CLOEXEC);
    in_w_ = in[1], out_r_ = out[0];
    std::signal(SIGPIPE, SIG_IGN);  // a dead worker must not kill us on write
    reader_ = std::thread([this] { reader(); });
}

Subprocess::~Subprocess() {
    kill();
    if (in_w_ >= 0) close(in_w_);
    if (reader_.joinable()) reader_.join();
    if (out_r_ >= 0) close(out_r_);
    if (pid_ > 0) waitpid(pid_, nullptr, 0);
}

void Subprocess::write(const std::string& b) {
    std::size_t off = 0;
    while (off < b.size()) {
        ssize_t w = ::write(in_w_, b.data() + off, b.size() - off);
        if (w <= 0) throw std::runtime_error("worker pipe closed");
        off += std::size_t(w);
    }
}

void Subprocess::reader() {
    char tmp[65536];
    for (;;) {
        ssize_t n = ::read(out_r_, tmp, sizeof tmp);
        if (n <= 0) break;
        std::lock_guard lk(mu_);
        buf_.append(tmp, std::size_t(n));
        cv_.notify_all();
    }
    std::lock_guard lk(mu_);
    eof_ = true;
    cv_.notify_all();
}

void Subprocess::kill() {
    if (pid_ > 0) ::kill(pid_, SIGKILL);
}

bool Subprocess::alive() { return pid_ > 0 && waitpid(pid_, nullptr, WNOHANG) == 0; }
#endif

bool Subprocess::read(std::size_t n, std::string& out, std::chrono::milliseconds timeout, bool* timed_out) {
    std::unique_lock lk(mu_);
    bool ok = cv_.wait_for(lk, timeout, [&] { return buf_.size() >= n || eof_; });
    if (timed_out) *timed_out = !ok;
    if (buf_.size() < n) return false;
    out.assign(buf_, 0, n);
    buf_.erase(0, n);
    return true;
}

}  // namespace anvil
