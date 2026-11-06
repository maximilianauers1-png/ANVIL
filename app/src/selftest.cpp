#include "selftest.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>

#include <cmath>

#include "app_controller.hpp"
#include "viewport_item.hpp"

namespace anvil {

SelfTest::SelfTest(QQuickWindow* w, ViewportItem* v, AppController* c, QString out_dir)
    : win_(w), view_(v), app_(c), out_(std::move(out_dir)) {
    QDir().mkpath(out_);
    connect(win_, &QQuickWindow::frameSwapped, this, [this] {
        ++frames_;
        if (radial_press_frame_ >= 0 && radial_frames_ < 0 && view_->radialActive())
            radial_frames_ = frames_ - radial_press_frame_;
    }, Qt::QueuedConnection);

    const QString stl = QDir(out_).filePath("ui.stl");
    auto origin = [this] { return project(0, 0, 0); };

    add("wait for first frames", [] {}, [this] { return frames_ > 5 && view_->deviceName() != ""; }, 20000);
    add("screenshot start", [this] { screenshot("01_start"); });

    // 1) hover the origin: snapping shows "Origin"
    add("hover origin snaps", [this, origin] { mouseMove(origin() + QPointF(3, -2)); },
        [this] { return view_->snapLabel() == "Origin"; });

    // 2) radial flick east (empty context, sector 0 = Cylinder). Press, move and
    //    release are delivered back-to-back: the flick completes before any frame.
    add("radial flick creates Cylinder1", [this, origin] {
        radial_press_frame_ = frames_;
        flick(origin(), 0, 70);
    }, [this] { return idle() && hasObject("Cylinder1"); });
    add("radial menu was drawable in <= 1 frame", [this] {
        // the flick released before a frame was presented; measure the held case separately
        radial_press_frame_ = -1;
        radial_frames_ = -1;
        QPointF p = project(40, 40, 0);
        mouseMove(p);
        radial_press_frame_ = frames_;
        mousePress(p, Qt::RightButton);
    }, [this] { return radial_frames_ >= 0; });
    add("cancel held radial", [this] {
        check(radial_frames_ <= 1, QString("radial menu visible after %1 frame(s)").arg(radial_frames_));
        QPointF p = project(40, 40, 0);
        mouseRelease(p, Qt::RightButton);  // released in the dead zone = cancel
    }, [this] { return !view_->radialActive(); });

    // 3) select by clicking the cylinder, edit parameters in the panel fields
    add("select Cylinder1 by click", [this] { click(project(0, 0, 10)); },
        [this] { return app_->selection() == QStringList{"Cylinder1"}; });
    add("type d = 25", [this] { typeInto("param__d", "25"); }, [this] { return idle(); });
    add("type h = 3", [this] { typeInto("param__h", "3"); }, [this] { return idle(); });
    add("type chamfer_top = 0.6", [this] { typeInto("param__chamfer_top", "0.6"); }, [this] { return idle(); });
    add("screenshot base", [this] { screenshot("02_base"); });

    // 4) bottom view via radial submenu: View (sector 7, SE) -> Bottom (sector 3, NW)
    add("deselect", [this] { click(project(22, -22, 0)); },
        [this] { return app_->selection().isEmpty(); });
    add("radial submenu View > Bottom", [this] {
        QPointF c0 = project(22, -22, 0);
        mouseMove(c0);
        mousePress(c0, Qt::RightButton);
        double a = -45 * M_PI / 180;  // sector 7 (south-east)
        QPointF p1 = c0 + QPointF(std::cos(a) * 60, -std::sin(a) * 60);
        QPointF p2 = c0 + QPointF(std::cos(a) * 130, -std::sin(a) * 130);  // beyond submenu radius -> opens View
        mouseMove(p1, Qt::RightButton);
        mouseMove(p2, Qt::RightButton);
        double b = 135 * M_PI / 180;  // sector 3 (north-west) in the submenu
        QPointF p3 = p2 + QPointF(std::cos(b) * 70, -std::sin(b) * 70);
        mouseMove(p3, Qt::RightButton);
        mouseRelease(p3, Qt::RightButton);
    }, [this] { return idle() && view_->camForward().z() > 0.999f; }, 10000);
    add("screenshot bottom", [this] { screenshot("03_bottom"); });

    // 5) hover the bottom center (snap), flick east -> Cylinder2 at (0,0,0)
    add("hover bottom center", [this] { mouseMove(project(0, 0, 0) + QPointF(2, 2)); },
        [this] { return view_->snapLabel() == "Origin" || view_->snapLabel() == "Center"; });
    add("radial flick creates Cylinder2", [this] { flick(project(0, 0, 0) + QPointF(2, 2), 0, 70); },
        [this] { return idle() && hasObject("Cylinder2"); });

    // 6) select Cylinder2 in the scene list and edit it
    add("select Cylinder2 in list", [this] { clickItem("obj_Cylinder2"); },
        [this] { return app_->selection() == QStringList{"Cylinder2"}; });
    add("type d = 5.1", [this] { typeInto("param__d", "5.1"); }, [this] { return idle(); });
    add("type h = 2.2", [this] { typeInto("param__h", "2.2"); }, [this] { return idle(); });

    // 7) select both (Ctrl+click) and subtract via the radial menu (north = Subtract)
    add("select Cylinder1", [this] { clickItem("obj_Cylinder1"); },
        [this] { return app_->selection() == QStringList{"Cylinder1"}; });
    add("ctrl-select Cylinder2", [this] { clickItem("obj_Cylinder2", Qt::ControlModifier); },
        [this] { return app_->selection() == QStringList({"Cylinder1", "Cylinder2"}); });
    add("radial flick north subtracts", [this] { flick(project(40, 40, 0), 90, 70); },
        [this] { return idle() && hasObject("Cylinder1") && !hasObject("Cylinder2"); });
    add("iso view", [this] { app_->requestView("iso"); }, [this] { return idle(); });
    add("screenshot result", [this] { screenshot("04_result"); });

    // 8) export through the dialog (radial south = Export STL…)
    add("select result", [this] { clickItem("obj_Cylinder1"); },
        [this] { return app_->selection() == QStringList{"Cylinder1"}; });
    add("radial flick south opens export", [this] { flick(project(40, 40, 0), -90, 70); },
        [this] { return item("exportPath") != nullptr; });
    add("export dialog checks pass", [this, stl] {
        typeInto("exportPath", stl);
        screenshot("05_export_dialog");
    }, [this] {
        auto* s = item("exportCheckSummary");
        return s && s->property("text").toString().startsWith(QString::fromUtf8("✓"));
    });
    add("click Export", [this] { clickItem("exportConfirm"); }, [stl, this] { return idle() && QFile::exists(stl); });

    // 9) undo/redo via the top bar buttons
    add("undo via button", [this] { clickItem("btnUndo"); }, [this] { return idle() && hasObject("Cylinder2"); });
    add("redo via button", [this] { clickItem("btnRedo"); }, [this] { return idle() && !hasObject("Cylinder2"); });
    // 10) live theme switch + unit display
    add("light theme", [this] { clickItem("theme_light"); }, [this] { return idle() && app_->theme() == "light"; });
    add("unit inch", [this] { clickItem("unit_in"); }, [this] { return idle() && app_->unit() == "in"; });
    add("screenshot light", [this] { screenshot("06_light_inch"); });
    add("studio theme + mm + mesh view", [this] {
        clickItem("theme_studio");
        clickItem("unit_mm");
        clickItem("btnMesh");
    }, [this] { return idle() && app_->theme() == "studio" && app_->unit() == "mm" && app_->display() == "mesh"; });
    add("screenshot studio mesh", [this] { screenshot("07_studio_mesh"); });

    // 11) draw a rectangle on the top face (radial NW = Rectangle), edit it, cut it through
    add("shaded, top view, deselect", [this] {
        clickItem("btnMesh");
        app_->setSelection({});
        app_->requestView("top");
    }, [this] { return idle() && app_->display() == "shaded" && view_->camForward().z() < -0.999f; }, 10000);
    add("radial flick NW starts Rectangle", [this] { flick(view_->mapToScene(QPointF(140, 180)), 135, 70); },
        [this] { return view_->tool() == "draw" && view_->drawKind() == "rect"; });
    add("rectangle: two corners on the top face", [this] {
        click(project(-4.3, -1.7, 3));
        click(project(4.3, 1.7, 3));
    }, [this] { return idle() && hasObject("Sketch1") && app_->selection() == QStringList{"Sketch1"}; });
    add("sketch lies on the top face (z = 3 mm)", [this] {
        QVariantMap info = app_->sketchInfo("Sketch1");
        check(info["plane"].toString() == "XY" && std::abs(info["offset"].toDouble() - 3.0) < 1e-9,
              "sketch plane XY at offset " + info["offset"].toString());
        check(app_->sketchTarget("Sketch1") == "Cylinder1", "sketch remembers its part: " + app_->sketchTarget("Sketch1"));
    });
    add("zoom to the part", [this] { app_->requestView("frame"); }, [this] { return idle(); });
    add("open sketch editor", [this] { clickItem("sketchEdit"); }, [this] { return item("sketchDone") != nullptr; });
    add("click the measured length of L1", [this] { clickItem("measLabel_L1"); },
        [this] { return item("measure_L1") != nullptr; });
    add("type 12 -> length dimension", [this] { typeInto("measure_L1", "12"); }, [this] {
        if (!idle()) return false;
        for (const auto& c : app_->sketchInfo("Sketch1")["curves"].toList())
            if (c.toMap()["id"] == "L1") return std::abs(c.toMap()["length"].toDouble() - 12.0) < 1e-6;
        return false;
    });
    add("screenshot sketch edit", [this] { screenshot("08_sketch_edit"); });
    add("close sketch editor", [this] { clickItem("sketchDone"); }, [this] { return item("sketchCut") != nullptr; });
    add("cut through the part (one click)", [this] {
        vol_before_ = objectValue("Cylinder1", "volume");
        clickItem("sketchCut");
    }, [this] {
        return idle() && objectValue("Cylinder1", "volume") > 0 &&
               objectValue("Cylinder1", "volume") < vol_before_ - 1.0;
    }, 15000);

    // 12) polyline on the ground, solidify it into a trace, 'Cut with...' by picking
    add("radial flick N starts Polyline", [this] {
        app_->setSelection({});
        flick(view_->mapToScene(QPointF(140, 180)), 90, 70);
    }, [this] { return view_->tool() == "draw" && view_->drawKind() == "polyline"; });
    add("polyline: three points, Enter", [this] {
        click(project(-14, -3, 0));
        click(project(0, -3, 0));
        click(project(0, 6, 0));
        key(Qt::Key_Return);
    }, [this] { return idle() && hasObject("Sketch2") && app_->selection() == QStringList{"Sketch2"}; });
    add("solidify (quick bar)", [this] { clickItem("sketchSolidify"); },
        [this] { return idle() && hasObject("Solid1") && objectValue("Solid1", "volume") > 0; }, 15000);
    add("select part, radial N = Cut with...", [this] {
        app_->setSelection({"Cylinder1"});
        vol_before_ = objectValue("Cylinder1", "volume");
        flick(view_->mapToScene(QPointF(140, 180)), 90, 70);
    }, [this] { return view_->tool() == "pick"; });
    add("pick the trace, Enter cuts", [this] {
        click(project(-13.3, -3, 1));
        key(Qt::Key_Return);
    }, [this] {
        return idle() && !hasObject("Solid1") && objectValue("Cylinder1", "volume") < vol_before_ - 0.1;
    }, 15000);
    add("iso view for the result", [this] { app_->requestView("iso"); }, [this] { return idle(); });
    add("screenshot mask cut", [this] { screenshot("09_mask_cut"); });

    // 13) deform from the radial (selection ring, sector 3), scrub 'twist' in the panel
    add("radial NW = Deform", [this] {
        app_->setSelection({"Cylinder1"});
        flick(view_->mapToScene(QPointF(140, 180)), 135, 70);
    }, [this] { return idle() && item("scrub__twist") != nullptr; });
    add("scrub twist +60 px = 30 deg", [this] {
        QQuickItem* it = item("scrub__twist");
        QPointF c = center(it);
        mouseMove(c);
        mousePress(c, Qt::LeftButton);
        mouseMove(c + QPointF(30, 0), Qt::LeftButton);
        mouseMove(c + QPointF(60, 0), Qt::LeftButton);
        mouseRelease(c + QPointF(60, 0), Qt::LeftButton);
    }, [this] {
        auto h = app_->history();
        return idle() && !h.isEmpty() && h.back().toMap()["source"].toString().contains("twist=30*deg");
    });
    add("screenshot deform", [this] { screenshot("10_deform"); });

    // 15) Sculpt mode: make the part sculptable, one draw stroke = one sculpt(...) command
    add("sculpt mode + make sculptable", [this] {
        app_->requestScenery("none");
        app_->setMode("Sculpt");
        app_->setSelection({"Cylinder1"});
        app_->requestView("top");
    }, [this] { return idle() && item("makeSculptable") != nullptr; }, 10000);
    add("click Make sculptable", [this] { clickItem("makeSculptable"); },
        [this] { return idle() && app_->objectKind("Cylinder1") == "Sculpt" && view_->sculptReady("Cylinder1"); }, 60000);
    add("draw stroke across the top", [this] {
        vol_before_ = objectValue("Cylinder1", "volume");
        QPointF a = project(-8, 6, 3), b = project(8, 6, 3);
        mouseMove(a);
        mousePress(a, Qt::LeftButton);
        for (int i = 1; i <= 12; ++i) mouseMove(a + (b - a) * (i / 12.0), Qt::LeftButton);
        mouseRelease(b, Qt::LeftButton);
    }, [this] {
        auto h = app_->history();
        return idle() && !h.isEmpty() && h.back().toMap()["source"].toString().startsWith("sculpt(\"Cylinder1\", \"draw\"") &&
               objectValue("Cylinder1", "volume") > vol_before_ + 0.1;
    }, 60000);
    add("two strokes back to back (clay, mirrored)", [this] {
        vol_before_ = objectValue("Cylinder1", "volume");
        hist_before_ = app_->history().size();
        view_->setProperty("sculptBrush", "clay");
        view_->setProperty("sculptSymmetry", "x");
        for (double y : {-6.0, -9.0}) {  // the second starts before the first is evaluated
            QPointF a = project(3, y, 3), b = project(8, y, 3);
            mouseMove(a);
            mousePress(a, Qt::LeftButton);
            for (int i = 1; i <= 8; ++i) mouseMove(a + (b - a) * (i / 8.0), Qt::LeftButton);
            mouseRelease(b, Qt::LeftButton);
        }
    }, [this] {
        auto h = app_->history();
        if (!idle() || h.size() < hist_before_ + 2) return false;
        const QString last = h.back().toMap()["source"].toString(), prev = h[h.size() - 2].toMap()["source"].toString();
        return last.contains("\"clay\"") && prev.contains("\"clay\"") && last.contains("symmetry=\"x\"") &&
               objectValue("Cylinder1", "volume") > vol_before_ + 0.1 && view_->sculptObjects().contains("Cylinder1");
    }, 60000);
    add("iso view for sculpt", [this] { app_->requestView("iso"); }, [this] { return idle(); });
    add("screenshot sculpt", [this] { screenshot("12_sculpt"); });
    add("back to Design", [this] { app_->setMode("Design"); }, [this] { return !view_->sculptMode(); });

    // 14) Studio scenery: sunset, fjord, night (procedural environment pass)
    for (const char* sc : {"sunset", "fjord", "night"}) {
        const QString name = sc;
        add("scenery " + name, [this, name] {
            app_->setSelection({});
            app_->requestScenery(name);
            app_->requestViewDirection(QVector3D(0.94f, 0.34f, 0.10f));  // low, towards the sun/moon
        }, [this, name] { return idle() && app_->scenery() == name && std::abs(view_->camForward().z() + 0.1f) < 0.02f; });
        add("screenshot scenery " + name, [this, name] { screenshot("11_scenery_" + name); });
    }

    connect(&timer_, &QTimer::timeout, this, &SelfTest::tick);
    timer_.start(50);
    step_clock_.start();
}

void SelfTest::add(QString name, std::function<void()> act, std::function<bool()> done, int timeout) {
    steps_.push_back({std::move(name), std::move(act), std::move(done), timeout});
}

void SelfTest::check(bool ok, const QString& what) {
    QJsonObject o{{"check", what}, {"ok", ok}};
    log_.append(o);
    if (!ok) failed_ = true;
}

void SelfTest::tick() {
    if (cur_ >= steps_.size()) return finish();
    Step& s = steps_[cur_];
    if (!started_) {
        started_ = true;
        step_clock_.restart();
        s.act();
        win_->update();
        return;
    }
    if (!s.done || s.done()) {
        std::printf("selftest: ok   %-45s %6lld ms\n", s.name.toUtf8().constData(), step_clock_.elapsed());
        std::fflush(stdout);
        log_.append(QJsonObject{{"step", s.name}, {"ok", true}, {"ms", double(step_clock_.elapsed())}});
        ++cur_;
        started_ = false;
        return;
    }
    if (step_clock_.elapsed() > s.timeout_ms) {
        std::printf("selftest: FAIL %s (last error: %s)\nconsole tail:\n%s\n", s.name.toUtf8().constData(),
                    app_->lastError().toUtf8().constData(), app_->consoleText().right(600).toUtf8().constData());
        std::fflush(stdout);
        log_.append(QJsonObject{{"step", s.name}, {"ok", false}, {"error", "timeout; last error: " + app_->lastError()}});
        failed_ = true;
        screenshot("failed");
        return finish();
    }
    win_->update();
}

void SelfTest::finish() {
    timer_.stop();
    QJsonObject r;
    r["ok"] = !failed_;
    r["steps"] = log_;
    r["radial_frames_to_visible"] = radial_frames_;
    r["history"] = QJsonArray::fromVariantList(app_->history());
    QFile f(QDir(out_).filePath("report.json"));
    if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(r).toJson());
    std::printf("selftest %s\n", failed_ ? "FAILED" : "OK");
    std::fflush(stdout);
    QTimer::singleShot(200, qApp, &QCoreApplication::quit);
}

// ------------------------------------------------------------------ helpers
QQuickItem* SelfTest::item(const QString& name) const {
    std::function<QQuickItem*(QQuickItem*)> find = [&](QQuickItem* it) -> QQuickItem* {
        if (it->objectName() == name && it->isVisible()) return it;
        for (QQuickItem* c : it->childItems())
            if (auto* r = find(c)) return r;
        return nullptr;
    };
    if (auto* r = find(win_->contentItem())) return r;
    // popups live in the overlay layer
    return nullptr;
}

QPointF SelfTest::center(QQuickItem* it) const { return it->mapToScene(QPointF(it->width() / 2, it->height() / 2)); }

QPointF SelfTest::project(double x, double y, double z) const {
    return view_->mapToScene(view_->projectPoint({x, y, z}));
}

void SelfTest::mouseMove(QPointF p, Qt::MouseButtons held) {
    QMouseEvent e(QEvent::MouseMove, p, p, win_->mapToGlobal(p), Qt::NoButton, held, QGuiApplication::keyboardModifiers());
    QCoreApplication::sendEvent(win_, &e);
}
void SelfTest::mousePress(QPointF p, Qt::MouseButton b, Qt::KeyboardModifiers m) {
    QMouseEvent e(QEvent::MouseButtonPress, p, p, win_->mapToGlobal(p), b, b, m);
    QCoreApplication::sendEvent(win_, &e);
}
void SelfTest::mouseRelease(QPointF p, Qt::MouseButton b, Qt::KeyboardModifiers m) {
    QMouseEvent e(QEvent::MouseButtonRelease, p, p, win_->mapToGlobal(p), b, Qt::NoButton, m);
    QCoreApplication::sendEvent(win_, &e);
}
void SelfTest::click(QPointF p, Qt::KeyboardModifiers m) {
    mouseMove(p);
    mousePress(p, Qt::LeftButton, m);
    mouseRelease(p, Qt::LeftButton, m);
}
void SelfTest::clickItem(const QString& name, Qt::KeyboardModifiers m) {
    QQuickItem* it = item(name);
    if (!it) {
        check(false, "item '" + name + "' not found");
        return;
    }
    click(center(it), m);
}
void SelfTest::key(int k, Qt::KeyboardModifiers m, const QString& text) {
    QKeyEvent p(QEvent::KeyPress, k, m, text), r(QEvent::KeyRelease, k, m, text);
    QCoreApplication::sendEvent(win_, &p);
    QCoreApplication::sendEvent(win_, &r);
}
void SelfTest::typeInto(const QString& name, const QString& text) {
    QQuickItem* it = item(name);
    if (!it) {
        check(false, "field '" + name + "' not found");
        return;
    }
    click(center(it));
    it->forceActiveFocus();
    key(Qt::Key_A, Qt::ControlModifier);
    for (QChar ch : text) {
        int k = ch.isDigit() ? int(Qt::Key_0) + ch.digitValue() : ch == '.' ? int(Qt::Key_Period) : int(ch.toUpper().unicode());
        key(k, Qt::NoModifier, QString(ch));
    }
    key(Qt::Key_Return);
}
void SelfTest::flick(QPointF from, double angle_deg, double dist, Qt::MouseButton b) {
    const double a = angle_deg * M_PI / 180;
    mouseMove(from);
    mousePress(from, b);
    QPointF to = from + QPointF(std::cos(a) * dist, -std::sin(a) * dist);
    mouseMove((from + to) / 2, b);
    mouseRelease(to, b);
}
void SelfTest::screenshot(const QString& name) { win_->grabWindow().save(QDir(out_).filePath(name + ".png")); }
bool SelfTest::idle() const { return !app_->busy(); }
double SelfTest::objectValue(const QString& name, const QString& key) const {
    for (const auto& o : app_->objects())
        if (o.toMap()["name"].toString() == name) return o.toMap()[key].toDouble();
    return -1;
}
bool SelfTest::hasObject(const QString& name) const {
    for (const auto& o : app_->objects())
        if (o.toMap()["name"].toString() == name) return true;
    return false;
}

}  // namespace anvil
