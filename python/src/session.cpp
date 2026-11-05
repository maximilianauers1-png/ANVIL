#include "session.hpp"

#include <pybind11/embed.h>

#include <stdexcept>

#include "anvil/fsutil.hpp"
#include "anvil/version.hpp"

namespace py = pybind11;

namespace anvil {

namespace {
thread_local Session* g_current = nullptr;

ExecResult from_dict(const py::dict& d) {
    ExecResult r;
    r.ok = d.contains("ok") && d["ok"].cast<bool>();
    if (d.contains("output")) r.output = d["output"].cast<std::string>();
    if (d.contains("error")) r.error = d["error"].cast<std::string>();
    if (d.contains("line")) r.error_line = d["line"].cast<int>();
    if (d.contains("col")) r.error_col = d["col"].cast<int>();
    if (d.contains("json")) r.json = d["json"].cast<std::string>();
    return r;
}
}  // namespace

void register_embedded_module();  // bindings.cpp

Session* Session::current() { return g_current; }

Session::Session(SessionOptions opt) : opt_(std::move(opt)) {
    register_embedded_module();
    if (!opt_.journal_path.empty()) journal_ = std::make_unique<Journal>(opt_.journal_path);
    auto ready = ready_.get_future();
    py_thread_ = std::thread([this] { python_main(); });
    ready.wait();
    if (!init_error_.empty()) {
        {
            std::lock_guard lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        py_thread_.join();
        throw std::runtime_error("Python runtime failed to start: " + init_error_);
    }
}

Session::~Session() {
    {
        std::lock_guard lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (py_thread_.joinable()) py_thread_.join();
}

void Session::close_clean() {
    post([this] {
        if (journal_) journal_->close_clean();
    });
}

void Session::python_main() {
    g_current = this;
    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    config.install_signal_handlers = 0;  // never steal Ctrl+C / SIGINT from the host
    config.user_site_directory = 0;      // reproducible: ignore ~/.local site-packages
    config.write_bytecode = 0;           // never write .pyc into the install directory
    std::unique_ptr<py::scoped_interpreter> guard;
    try {
        guard = std::make_unique<py::scoped_interpreter>(&config, 0, nullptr, false);
        py::module_ sys = py::module_::import("sys");
        sys.attr("path").attr("insert")(0, opt_.python_dir.string());
        py::module_ rt = py::module_::import("anvil._runtime");
        rt.attr("init")(opt_.plugin_dir.string());
    } catch (const std::exception& e) {
        init_error_ = e.what();
        ready_.set_value();
        return;
    }
    ready_.set_value();
    for (;;) {
        Task t;
        {
            std::unique_lock lk(mu_);
            cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
            if (queue_.empty()) break;  // stop requested and drained
            t = std::move(queue_.front());
            queue_.pop_front();
        }
        t.fn();
    }
    g_current = nullptr;
}

void Session::post(std::function<void()> fn) {
    {
        std::lock_guard lk(mu_);
        queue_.push_back({std::move(fn)});
    }
    cv_.notify_one();
}

std::future<ExecResult> Session::submit(std::string source, std::string origin, ExecMode mode) {
    auto p = std::make_shared<std::promise<ExecResult>>();
    auto f = p->get_future();
    post([this, p, source = std::move(source), origin = std::move(origin), mode] {
        try {
            p->set_value(exec_on_python_thread(source, origin, mode));
        } catch (...) {
            p->set_exception(std::current_exception());
        }
    });
    return f;
}

void Session::submit_async(std::string source, std::string origin, ExecMode mode,
                           std::function<void(ExecResult)> done) {
    post([this, source = std::move(source), origin = std::move(origin), mode, done = std::move(done)] {
        ExecResult r;
        try {
            r = exec_on_python_thread(source, origin, mode);
        } catch (const std::exception& e) {
            r.ok = false;
            r.error = e.what();
        }
        if (done) done(std::move(r));
    });
}

ExecResult Session::run(const std::string& source, const std::string& origin, ExecMode mode) {
    return submit(source, origin, mode).get();
}

std::string Session::check_state_evaluates() {
    // Every object whose geometry changed in this transaction must evaluate;
    // otherwise the command fails and is rolled back (no broken state in history).
    auto before = doc_.state();
    auto now = doc_.current();
    bool vars_changed = before->vars != now->vars;
    auto vars = now->resolver();
    EvalSettings es;
    es.chord_mm = opt_.display_chord_mm;
    for (const auto& o : now->objects) {
        const Object* old = before->find(o->name);
        if (!vars_changed && old && old->root == o->root) continue;
        try {
            evaluate(o->root, vars, es, &cache_);
        } catch (const std::exception& e) {
            return "object '" + o->name + "': " + e.what();
        }
    }
    return {};
}

ExecResult Session::exec_on_python_thread(const std::string& source, const std::string& origin, ExecMode mode) {
    py::module_ rt = py::module_::import("anvil._runtime");
    ExecResult r;
    switch (mode) {
        case ExecMode::Command: {
            std::uint64_t seq = journal_ ? journal_->begin_txn(origin, source) : 0;
            doc_.begin(source, origin);
            try {
                // console: REPL conveniences (print values, show a bare solid)
                r = from_dict(rt.attr("run")(source, origin == "console" ? "console" : "exec"));
            } catch (const std::exception& e) {
                r.ok = false;
                r.error = e.what();
            }
            if (r.ok) {
                std::string err = check_state_evaluates();
                if (!err.empty()) r.ok = false, r.error = err;
            }
            if (r.ok) {
                r.changed = doc_.commit();
                if (journal_) journal_->mark_ok(seq);
            } else {
                doc_.rollback();
                if (journal_) journal_->mark_failed(seq);
            }
            r.seq = seq;
            break;
        }
        case ExecMode::Preview: {
            preview_mode_ = true;
            preview_.reset();
            doc_.begin("preview", "preview");
            try {
                r = from_dict(rt.attr("run")(source, "preview"));
            } catch (const std::exception& e) {
                r.ok = false;
                r.error = e.what();
            }
            doc_.rollback();
            preview_mode_ = false;
            r.preview = preview_;
            if (r.preview) {
                try {
                    EvalSettings es;
                    es.chord_mm = opt_.display_chord_mm;
                    evaluate(r.preview, doc_.state()->resolver(), es, &cache_);
                } catch (const std::exception& e) {
                    r.ok = false, r.error = e.what(), r.preview.reset();
                }
            }
            break;
        }
        case ExecMode::Check:
            r = from_dict(rt.attr("check")(source));
            break;
        case ExecMode::Complete:
            r = from_dict(rt.attr("complete")(source));
            break;
        case ExecMode::Query:
            r = from_dict(rt.attr("query")(source));
            break;
    }
    return r;
}

std::future<bool> Session::undo() {
    auto p = std::make_shared<std::promise<bool>>();
    auto f = p->get_future();
    post([this, p] {
        bool ok = doc_.undo();
        if (ok && journal_) journal_->mark_undo();
        p->set_value(ok);
    });
    return f;
}

std::future<bool> Session::redo() {
    auto p = std::make_shared<std::promise<bool>>();
    auto f = p->get_future();
    post([this, p] {
        bool ok = doc_.redo();
        if (ok && journal_) journal_->mark_redo();
        p->set_value(ok);
    });
    return f;
}

RecoveryReport Session::replay(const std::filesystem::path& journal) {
    JournalContents c = Journal::read(journal);
    auto p = std::make_shared<std::promise<RecoveryReport>>();
    auto f = p->get_future();
    post([this, p, c] {
        RecoveryReport rep;
        rep.truncated_tail = c.truncated_tail;
        replaying_ = true;  // suppresses side effects (exports); the new journal still records everything
        for (const auto& a : c.actions) {
            if (a.kind == JournalAction::Undo) {
                if (doc_.undo() && journal_) journal_->mark_undo();
                continue;
            }
            if (a.kind == JournalAction::Redo) {
                if (doc_.redo() && journal_) journal_->mark_redo();
                continue;
            }
            if (a.failed) {
                ++rep.skipped_failed;
                continue;
            }
            if (!a.ok) {
                rep.unfinished.push_back(a.source);
                continue;
            }
            ExecResult r = exec_on_python_thread(a.source, a.origin, ExecMode::Command);
            if (r.ok) ++rep.replayed;
            else rep.errors.push_back("#" + std::to_string(a.seq) + ": " + r.error);
        }
        replaying_ = false;
        p->set_value(rep);
    });
    return f.get();
}

void Session::save(const std::filesystem::path& path) {
    write_file_atomic(path, doc_.state()->to_python("saved by ANVIL"));
}

ExecResult Session::open(const std::filesystem::path& path) {
    std::string content = read_file(path);
    // Format check + migration hook (ADR 0014): "# anvil-format: N" on line 1.
    int fmt = 0;
    if (content.rfind("# anvil-format:", 0) == 0) fmt = std::atoi(content.c_str() + 15);
    if (fmt > kDocumentFormat) {
        ExecResult r;
        r.error = "'" + path.filename().string() + "' was written by a newer ANVIL (document format " +
                  std::to_string(fmt) + ", this version reads <= " + std::to_string(kDocumentFormat) + ")";
        return r;
    }
    std::string src = "# open: " + path.filename().string() + "\nclear()\n" + content;
    return run(src, "open");
}

}  // namespace anvil
