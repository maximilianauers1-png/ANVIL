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
#include <QVersionNumber>
#include <QVulkanInstance>

#include <chrono>
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
        if (!probe.create()) {
            std::fprintf(stderr, "ANVIL needs Vulkan 1.1+ (driver missing or GPU unsupported): error %d\n",
                         int(probe.errorCode()));
            return 2;
        }
    }

    const QString sessions = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/sessions";
    const QString session_dir = sessions + "/" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") + "-" +
                                QString::number(QCoreApplication::applicationPid());
    anvil::SessionOptions opt;
    opt.python_dir = anvil::default_python_dir();
    opt.plugin_dir = (QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/plugins").toStdString();

    std::unique_ptr<anvil::AppController> ctrl;
    try {
        ctrl = std::make_unique<anvil::AppController>(opt, session_dir.toStdString());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ANVIL failed to start: %s\n", e.what());
        return 3;
    }

    qmlRegisterType<anvil::ViewportItem>("Anvil.Native", 1, 0, "Viewport");
    qmlRegisterSingletonInstance("Anvil.Native", 1, 0, "App", ctrl.get());

    QQmlApplicationEngine engine;
    engine.addImportPath("qrc:/qt/qml");
    engine.load(QUrl("qrc:/qt/qml/Anvil/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 4;
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
    auto* viewport = window->findChild<anvil::ViewportItem*>("viewport");
    if (!viewport) {
        std::fprintf(stderr, "internal error: viewport item not found\n");
        return 5;
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
