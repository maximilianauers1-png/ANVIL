// Session: the single command bus (ADR 0004/0005).
//
// Owns the Document, the shape cache, the journal and the embedded Python
// interpreter, which runs on its own thread. UI, scripts and (later) the AI
// submit Python source here; each submission is one transaction and, if it
// changes the document, exactly one undo step.
#pragma once
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "anvil/document.hpp"
#include "anvil/journal.hpp"
#include "anvil/validate.hpp"

namespace anvil {

enum class ExecMode {
    Command,   // journaled transaction (one undo step if it changes the document)
    Preview,   // evaluate an expression without changing the document (ghost preview)
    Check,     // compile only: syntax errors with line/column
    Complete,  // autocompletion candidates for the last token
    Query,     // read-only helper call into the Python runtime (returns JSON)
};

struct ExecResult {
    bool ok = false;
    bool changed = false;          // document changed (undo step created)
    std::string output;            // captured stdout/stderr
    std::string error;             // English, one line, precise
    int error_line = 0, error_col = 0;
    std::string json;              // Query/Complete results (JSON text)
    NodePtr preview;               // Preview: node to draw as ghost
    std::uint64_t seq = 0;         // journal sequence number
};

struct RecoveryReport {
    std::size_t replayed = 0, skipped_failed = 0;
    std::vector<std::string> unfinished;  // commands that never completed (possible crash cause)
    bool truncated_tail = false;
    std::vector<std::string> errors;
};

struct SessionOptions {
    std::filesystem::path python_dir;     // directory that contains the `anvil` package
    std::filesystem::path journal_path;   // empty: no journal (tests, CLI without --journal)
    std::filesystem::path plugin_dir;     // user plugins (optional)
    double display_chord_mm = 0.01;       // tol::kDefaultChordMm
};

class Session {
  public:
    explicit Session(SessionOptions opt);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    std::future<ExecResult> submit(std::string source, std::string origin, ExecMode mode = ExecMode::Command);
    /// Asynchronous variant: `done` is called on the Python thread with the result.
    void submit_async(std::string source, std::string origin, ExecMode mode, std::function<void(ExecResult)> done);
    /// Convenience: submit and wait.
    ExecResult run(const std::string& source, const std::string& origin = "script",
                   ExecMode mode = ExecMode::Command);

    std::future<bool> undo();
    std::future<bool> redo();

    /// Replays a journal into this session (one transaction per record).
    RecoveryReport replay(const std::filesystem::path& journal);
    /// Writes the document as a Python script (the .anvil format), atomically.
    void save(const std::filesystem::path& p);
    /// Replaces the document with the contents of an .anvil file (one undo step).
    ExecResult open(const std::filesystem::path& p);

    Document& document() { return doc_; }
    ShapeCache& cache() { return cache_; }
    const SessionOptions& options() const { return opt_; }
    void set_display_chord(double mm) { opt_.display_chord_mm = mm; }
    /// Marks the journal as cleanly closed (call on normal shutdown).
    void close_clean();

    /// Called (on the Python thread) for UI requests from scripts, e.g.
    /// view("top"), select([...]). The app marshals them to the GUI thread.
    std::function<void(const std::string& json)> ui_handler;

    // --- used by the Python bindings (Python thread only) ----------------------
    bool preview_mode() const { return preview_mode_; }
    bool replaying() const { return replaying_; }
    void set_preview(NodePtr n) { preview_ = std::move(n); }
    static Session* current();  // session whose Python thread is executing

  private:
    struct Task {
        std::function<void()> fn;
    };
    void python_main();
    void post(std::function<void()> fn);
    ExecResult exec_on_python_thread(const std::string& source, const std::string& origin, ExecMode mode);
    std::string check_state_evaluates();

    SessionOptions opt_;
    Document doc_;
    ShapeCache cache_{512};
    std::unique_ptr<Journal> journal_;
    std::thread py_thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Task> queue_;
    bool stop_ = false;
    bool preview_mode_ = false;
    bool replaying_ = false;
    NodePtr preview_;
    std::promise<void> ready_;
    std::string init_error_;
};

}  // namespace anvil
