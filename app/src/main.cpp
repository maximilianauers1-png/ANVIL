// ANVIL desktop application entry point.
//
//   anvil                         normal start
//   anvil file.anvil|file.stl     open/import at start
//   anvil --bench N --bench-out report.json [--bench-scene scene.stl]
//                                 orbit benchmark (G1), then quit
//   anvil --selftest OUTDIR       drive the real UI through the M1 acceptance (E2E)
//   anvil --measure-startup F     write the cold-start time (ms) to F and quit (gate G3)
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>
#include <QSysInfo>
#include <QVersionNumber>
#include <QVulkanInstance>

#include <chrono>
#include <exception>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <QFileInfo>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "app_controller.hpp"
#include "paths.hpp"
#include "selftest.hpp"
#include "viewport_item.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// ---------------------------------------------------------------- diagnostics
// A GUI process has no console: every Qt message goes to a log file, and fatal
// start-up errors are shown in a dialog (never a silent exit).
QFile* g_log = nullptr;
QStringList g_recent;  // last warnings, shown in the fatal dialog

void log_handler(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    static const char* names[] = {"debug", "warning", "critical", "fatal", "info"};
    QString line = QDateTime::currentDateTime().toString("HH:mm:ss.zzz") + " [" + names[int(type) % 5] + "] " + msg + "\n";
    if (g_log) {
        g_log->write(line.toUtf8());
        g_log->flush();
    }
    if (type != QtDebugMsg && type != QtInfoMsg) {
        g_recent << msg;
        if (g_recent.size() > 12) g_recent.removeFirst();
    }
    std::fputs(line.toLocal8Bit().constData(), stderr);
}

int fatal(int code, const QString& what) {
    QString text = what;
    if (!g_recent.isEmpty()) text += "\n\nDetails:\n" + g_recent.join("\n");
    if (g_log) text += "\n\nLog: " + g_log->fileName();
    qCritical("%s", qPrintable(what));
#ifdef _WIN32
    MessageBoxW(nullptr, reinterpret_cast<const wchar_t*>(text.utf16()), L"ANVIL could not start", MB_OK | MB_ICONERROR);
#endif
    std::fprintf(stderr, "%s\n", text.toLocal8Bit().constData());
    return code;
}

/// Milliseconds since the OS created this process (cold-start measurement, gate G3).
double process_age_ms() {
#ifdef _WIN32
    FILETIME c, e, k, u, now;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    GetSystemTimePreciseAsFileTime(&now);
    auto t = [](FILETIME f) { return (ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return double(t(now) - t(c)) / 1e4;
#else
    std::ifstream st("/proc/self/stat"), up("/proc/uptime");
    std::string s((std::istreambuf_iterator<char>(st)), {});
    auto rp = s.rfind(')');
    if (rp == std::string::npos) return 0;
    std::istringstream rest(s.substr(rp + 2));
    std::string tok;
    for (int i = 0; i < 20; ++i) rest >> tok;  // field 22: starttime (clock ticks since boot)
    double start_ticks = std::stod(tok);
    double uptime = 0;
    up >> uptime;
    return (uptime - start_ticks / double(sysconf(_SC_CLK_TCK))) * 1000.0;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    const double age_at_main = process_age_ms();
    const auto t_main = std::chrono::steady_clock::now();

    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName("ANVIL");
    QGuiApplication::setApplicationName("ANVIL");
    QQuickStyle::setStyle("Basic");

    {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        QDir().mkpath(dir);
        g_log = new QFile(dir + "/anvil.log", &app);
        if (g_log->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) qInstallMessageHandler(log_handler);
        else g_log = nullptr;
        qInfo("ANVIL %s starting (Qt %s, %s)", ANVIL_APP_VERSION, qVersion(), qPrintable(QSysInfo::prettyProductName()));
    }
    std::set_terminate([] {
        QString what = "unknown error";
        try {
            if (auto e = std::current_exception()) std::rethrow_exception(e);
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
        }
        fatal(6, "ANVIL stopped because of an unexpected error: " + what +
                     "\n\nYour work is in the journal and will be offered for recovery at the next start.");
        std::abort();
    });
    QStringList args = app.arguments();
    auto arg = [&](const QString& name) -> QString {
        int i = args.indexOf(name);
        return i >= 0 && i + 1 < args.size() ? args[i + 1] : QString();
    };
    const int bench_frames = arg("--bench").toInt();
    const QString bench_out = arg("--bench-out"), bench_scene = arg("--bench-scene"), selftest_dir = arg("--selftest");
    const QString startup_out = arg("--measure-startup");  // write cold-start ms to this file, then quit

    // Vulkan is required (ADR 0002); fail with a clear message instead of a black window.
    {
        QVulkanInstance probe;
        probe.setApiVersion(QVersionNumber(1, 1));
        if (!probe.create())
            return fatal(2, QString("ANVIL needs a Vulkan 1.1+ driver, but no Vulkan instance could be created "
                                    "(VkResult %1). Install or update the GPU driver (NVIDIA: GeForce driver).")
                                .arg(int(probe.errorCode())));
        qInfo("Vulkan instance OK (API %s)", qPrintable(probe.apiVersion().toString()));
    }

    const QString sessions = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/sessions";
    const QString session_dir = sessions + "/" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") + "-" +
                                QString::number(QCoreApplication::applicationPid());
#ifdef _WIN32
    // Python exits the process without a message when it finds no standard
    // library. Check the isolated search path (ANVIL._pth) first and say why.
    {
        const QDir exe(QCoreApplication::applicationDirPath());
        QFile pth(exe.filePath("ANVIL._pth"));
        if (pth.open(QIODevice::ReadOnly | QIODevice::Text)) {
            bool stdlib = false;
            QStringList tried;
            for (QString line : QString::fromUtf8(pth.readAll()).split('\n')) {
                line = line.trimmed();
                if (line.isEmpty() || line.startsWith('#')) continue;
                const QString path = QDir::isAbsolutePath(line) ? line : exe.filePath(line);
                tried << QDir::toNativeSeparators(path);
                const QFileInfo fi(path);
                if ((fi.isFile() && path.endsWith(".zip", Qt::CaseInsensitive)) ||
                    QFileInfo(path + "/encodings/__init__.py").isFile())
                    stdlib = true;
            }
            if (!stdlib)
                return fatal(3, "The Python 3.11 standard library was not found.\n\nSearched (ANVIL._pth):\n" +
                                    tried.join("\n") +
                                    "\n\nThe package is incomplete or ANVIL._pth was edited. Unpack the "
                                    "ANVIL-windows-x64 download again into an empty folder.");
        }
        if (!QFileInfo(exe.filePath("python311.dll")).isFile())
            qWarning("python311.dll is not next to ANVIL.exe; Windows will search PATH for it");
    }
#endif
    anvil::SessionOptions opt;
    opt.python_dir = anvil::default_python_dir();
    opt.plugin_dir = (QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/plugins").toStdString();

    std::unique_ptr<anvil::AppController> ctrl;
    try {
        ctrl = std::make_unique<anvil::AppController>(opt, session_dir.toStdString());
    } catch (const std::exception& e) {
        return fatal(3, QString("The geometry/Python session failed to start:\n") + e.what() +
                            "\n\nPython directory: " + QString::fromStdString(opt.python_dir.string()));
    }

    qmlRegisterType<anvil::ViewportItem>("Anvil.Native", 1, 0, "Viewport");
    qmlRegisterSingletonInstance("Anvil.Native", 1, 0, "App", ctrl.get());

    QQmlApplicationEngine engine;
    engine.addImportPath("qrc:/qt/qml");
    engine.load(QUrl("qrc:/qt/qml/Anvil/Main.qml"));
    if (engine.rootObjects().isEmpty()) return fatal(4, "The user interface (QML) failed to load.");
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
    QObject::connect(window, &QQuickWindow::sceneGraphError, &app, [&](QQuickWindow::SceneGraphError, const QString& msg) {
        fatal(7, "The Vulkan renderer could not be initialized:\n" + msg +
                     "\n\nUpdate the GPU driver; ANVIL needs Vulkan 1.1 or newer.");
        QCoreApplication::exit(7);
    });
    auto* viewport = window->findChild<anvil::ViewportItem*>("viewport");
    if (!viewport) {
        return fatal(5, "Internal error: viewport item not found.");
    }
    ctrl->attachViewport(viewport);

    // Cold start = OS process creation -> first frame presented.
    QObject::connect(window, &QQuickWindow::frameSwapped, ctrl.get(), [&, done = false]() mutable {
        if (done) return;
        done = true;
        double ms = age_at_main + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_main).count();
        ctrl->setStartupMs(ms);
        std::printf("startup_ms %.1f\n", ms);
        std::fflush(stdout);
        if (!startup_out.isEmpty()) {
            QFile f(startup_out);
            if (f.open(QIODevice::WriteOnly)) f.write(QByteArray::number(ms, 'f', 1));
            QTimer::singleShot(0, &app, &QCoreApplication::quit);
        }
    }, Qt::QueuedConnection);

    for (int i = 1; i < args.size(); ++i) {
        const QString& a = args[i];
        if (a.startsWith("--")) {
            ++i;  // skip option value
            continue;
        }
        QFileInfo fi(a);
        if (fi.suffix().toLower() == "anvil") ctrl->openDocument(fi.absoluteFilePath());
        else if (fi.exists()) ctrl->dropFiles({QUrl::fromLocalFile(fi.absoluteFilePath())});
    }

    if (bench_frames > 0) {
        if (!bench_scene.isEmpty()) ctrl->dropFiles({QUrl::fromLocalFile(QFileInfo(bench_scene).absoluteFilePath())});
        QObject::connect(viewport, &anvil::ViewportItem::benchmarkFinished, &app, [&](const QString& json) {
            std::printf("%s\n", json.toUtf8().constData());
            std::fflush(stdout);
            QTimer::singleShot(100, &app, &QCoreApplication::quit);
        });
        // start once the scene (incl. GPU upload) is in: poll the triangle count
        auto* poll = new QTimer(&app);
        QObject::connect(poll, &QTimer::timeout, [=, &ctrl]() {
            const qulonglong want = ctrl->sceneTriangles();
            if (bench_scene.isEmpty() || (want > 0 && viewport->triangles() >= want)) {
                poll->stop();
                viewport->runBenchmark(bench_frames, bench_out);
            } else {
                viewport->window()->update();
            }
        });
        poll->start(200);
    }

    std::unique_ptr<anvil::SelfTest> selftest;
    if (!selftest_dir.isEmpty()) selftest = std::make_unique<anvil::SelfTest>(window, viewport, ctrl.get(), selftest_dir);

    int rc = app.exec();
    if (selftest) rc = selftest->exitCode();
    engine.clearComponentCache();
    return rc;
}
