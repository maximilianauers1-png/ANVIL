// anvil-cli: headless runner for scripts, journals and batch exports.
//
//   anvil-cli script.py [more.py ...]   run each file as one command
//   anvil-cli -c "show(box())"          run inline code
//   anvil-cli --replay journal.py       replay a session journal (crash recovery)
//   anvil-cli --journal j.py ...        write a journal while running
//   anvil-cli --save out.anvil ...      save the resulting document
//   anvil-cli --print-doc ...           print the document as Python
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "anvil/fsutil.hpp"
#include "anvil/version.hpp"
#include "paths.hpp"
#include "session.hpp"

using namespace anvil;

int main(int argc, char** argv) {
    SessionOptions opt;
    opt.python_dir = default_python_dir();
    std::vector<std::pair<std::string, std::string>> jobs;  // (kind, arg)
    std::string save_path;
    bool print_doc = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value after " << a << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--version") {
            std::cout << "anvil " << version_string() << " (api " << kApiVersion << ")\n";
            return 0;
        } else if (a == "-h" || a == "--help") {
            std::cout << "usage: anvil-cli [--journal J] [--replay J] [-c CODE] [--save F] [--print-doc] [script.py ...]\n";
            return 0;
        } else if (a == "--journal") opt.journal_path = next();
        else if (a == "--replay") jobs.emplace_back("replay", next());
        else if (a == "-c") jobs.emplace_back("code", next());
        else if (a == "--save") save_path = next();
        else if (a == "--print-doc") print_doc = true;
        else jobs.emplace_back("file", a);
    }
    try {
        Session s(opt);
        int failures = 0;
        for (const auto& [kind, arg] : jobs) {
            if (kind == "replay") {
                RecoveryReport r = s.replay(arg);
                std::cout << "replayed " << r.replayed << " command(s), skipped " << r.skipped_failed
                          << " failed one(s)\n";
                for (const auto& u : r.unfinished) std::cout << "unfinished (possible crash cause): " << u << "\n";
                for (const auto& e : r.errors) std::cerr << "replay error " << e << "\n", ++failures;
                continue;
            }
            std::string src = kind == "code" ? arg : read_file(arg);
            ExecResult r = s.run(src, kind == "code" ? "console" : "script");
            std::cout << r.output;
            if (!r.ok) {
                std::cerr << (kind == "file" ? arg + ": " : std::string()) << r.error << "\n";
                ++failures;
            }
        }
        if (print_doc) std::cout << s.document().state()->to_python();
        if (!save_path.empty()) s.save(save_path);
        s.close_clean();
        return failures ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 3;
    }
}
