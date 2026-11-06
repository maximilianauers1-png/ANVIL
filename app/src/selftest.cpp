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
#include <QUrl>

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
    add("highlight a sector", [this] {
        QPointF p = project(40, 40, 0);
        mouseMove(p + QPointF(60, -35), Qt::RightButton);
    }, [this] { return view_->radialHighlight() >= 0; });
    add("screenshot radial gear", [this] { screenshot("02_radial"); });
    add("cancel held radial", [this] {
        check(radial_frames_ <= 1, QString("radial menu visible after %1 frame(s)").arg(radial_frames_));
        QPointF p = project(40, 40, 0);
        mouseMove(p, Qt::RightButton);
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
    add("type a brush size into the number box", [this] {
        QQuickItem* f = item("sculptRadiusField");
        if (!f) return;
        f->forceActiveFocus();
        QMetaObject::invokeMethod(f, "selectAll");
        for (QChar c : QString("1.5")) key(0, Qt::NoModifier, QString(c));
        key(Qt::Key_Return);
    }, [this] { return std::abs(view_->property("sculptRadius").toDouble() - 1.5) < 1e-9; });
    add("set size back to 3 mm", [this] { view_->setProperty("sculptRadius", 3.0); });
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
        if (view_->missingMeshes() > view_->missingNewMeshes()) ++flicker_samples_;  // a shown chunk must never blink out
        auto h = app_->history();
        if (!idle() || h.size() < hist_before_ + 2) return false;
        const QString last = h.back().toMap()["source"].toString(), prev = h[h.size() - 2].toMap()["source"].toString();
        return last.contains("\"clay\"") && prev.contains("\"clay\"") && last.contains("symmetry=\"x\"") &&
               objectValue("Cylinder1", "volume") > vol_before_ + 0.1 && view_->sculptObjects().contains("Cylinder1");
    }, 60000);
    add("no flicker during strokes", [this] { check(flicker_samples_ == 0, QString("sculpt chunks stayed visible during live strokes (%1 ticks with missing meshes, last %2)").arg(flicker_samples_).arg(view_->missingMeshes())); });
    add("texture stroke (scales), immediately repeated", [this] {
        vol_before_ = objectValue("Cylinder1", "volume");
        hist_before_ = app_->history().size();
        view_->setProperty("sculptBrush", "texture");
        view_->setProperty("sculptSymmetry", "");
        for (double x : {-6.0, -2.0, 2.0}) {
            QPointF a = project(x, -3, 3), b = project(x + 3, 3, 3);
            mouseMove(a);
            mousePress(a, Qt::LeftButton);
            for (int i = 1; i <= 10; ++i) mouseMove(a + (b - a) * (i / 10.0), Qt::LeftButton);
            mouseRelease(b, Qt::LeftButton);
        }
    }, [this] {
        if (view_->missingMeshes() > view_->missingNewMeshes()) ++flicker_samples_;
        auto h = app_->history();
        return idle() && h.size() >= hist_before_ + 3 && h.back().toMap()["source"].toString().contains("\"texture\"") &&
               view_->missingMeshes() == 0;
    }, 60000);
    add("no flicker during texture strokes", [this] { check(flicker_samples_ == 0, "chunks stayed visible during texture strokes"); });
    add("iso view for sculpt", [this] { app_->requestView("iso"); }, [this] { return idle(); });
    add("screenshot sculpt", [this] { screenshot("12_sculpt"); });
    add("back to Design", [this] { app_->setMode("Design"); }, [this] { return !view_->sculptMode(); });

    // 17) M4: import a rigged GLB (as from Tripo / Meshy) -> pose -> weapon -> merge for print
    if (const QString glb = qEnvironmentVariable("ANVIL_TEST_GLB"); !glb.isEmpty()) {
        add("import mini (drop GLB)", [this, glb] {
            app_->setMode("Mini");
            app_->setSelection({});
            for (const auto& o : app_->objects())
                if (o.toMap()["visible"].toBool()) app_->toggleVisible(o.toMap()["name"].toString());
            app_->dropFiles({QUrl::fromLocalFile(glb)});
        }, [this] { return idle() && hasObject("hero"); }, 60000);
        add("select imported mini", [this] { app_->setSelection({"hero"}); },
            [this] { QQuickItem* b = item("pose_attack"); return b && b->isVisible(); });
        add("pose imported mini: attack", [this] { clickItem("pose_attack"); }, [this] {
            auto h = app_->history();
            if (!idle() || h.isEmpty()) return false;
            const QString src = h.back().toMap()["source"].toString();
            return src.startsWith("pose(") && src.contains("hero") && src.contains("attack");
        }, 30000);
        add("equip zweihander (right hand)", [this] {
            clickItem("handR");
            clickItem("equip_zweihander");
        }, [this] { return idle() && hasObject("Zweihander1"); }, 60000);
        add("frame imported mini", [this] {
            app_->requestView("iso");
            view_->frameAll();
        }, [this] { return idle(); });
        add("screenshot imported mini", [this] { screenshot("14_mini_import"); });
        add("base 25 mm", [this] { clickItem("base25"); }, [this] { return idle() && hasObject("Base1"); }, 30000);
        add("merge for print", [this] {
            app_->setSelection({"hero", "Zweihander1", "Base1"});
            clickItem("miniMerge");
        }, [this] { return idle() && hasObject("Mini1") && view_->sculptObjects().contains("Mini1"); }, 180000);
        add("screenshot merged mini", [this] { screenshot("15_mini_merged"); });
    }
    add("back to Design (mini)", [this] { app_->setMode("Design"); }, [this] { return true; });

    // 18) Toolbox cut tools on a 20 x 20 x 10 mm box (top view: screen x/y = world x/y)
    add("cut tools: box", [this] {
        app_->setMode("Design");
        app_->setSelection({});
        for (const auto& o : app_->objects())
            if (o.toMap()["visible"].toBool()) app_->toggleVisible(o.toMap()["name"].toString());
        app_->execute("show(box(x=20*mm, y=20*mm, z=10*mm), name=\"CutBox\")", "ui");
    }, [this] { return idle() && hasObject("CutBox"); }, 30000);
    add("orthographic camera (parallel cuts: exact expected volumes)", [this] { app_->requestView("ortho"); },
        [this] { return idle() && view_->ortho(); });
    add("top view on the box", [this] { app_->requestView("top"); },
        [this] { return idle() && std::abs(view_->camForward().z() + 1.0f) < 1e-3f; });
    add("frame the box", [this] { view_->frameAll(); }, [this] { return idle(); });
    add("knife from the toolbox", [this] { clickItem("tool_knife"); },
        [this] { return view_->tool() == "cut" && view_->cutKind() == "knife"; });
    add("knife: path across, Enter", [this] {
        click(project(0, -11, 10));  // just beyond the box (frame-all fits it tightly)
        click(project(0, 11, 10));
        key(Qt::Key_Return);
        mouseMove(project(5, 0, 10));
    }, [this] { return view_->cutChoosing() && view_->cutHover() >= 0; });
    add("screenshot knife preview", [this] { screenshot("16_knife"); });
    add("knife: click the part to keep", [this] { click(project(5, 0, 10)); }, [this] {
        auto h = app_->history();
        return idle() && !h.isEmpty() && h.back().toMap()["source"].toString().startsWith("knife(\"CutBox\"") &&
               std::abs(objectValue("CutBox", "volume") - 2000) < 1e-6;
    }, 30000);
    add("lasso from the toolbox", [this] { clickItem("tool_lasso"); }, [this] { return view_->cutKind() == "lasso"; });
    add("lasso: drag a loop, keep the outside", [this] {
        const double sq[5][2] = {{3, -2}, {7, -2}, {7, 2}, {3, 2}, {3, -2}};
        QPointF a = project(sq[0][0], sq[0][1], 10);
        mouseMove(a);
        mousePress(a, Qt::LeftButton);
        for (int k = 0; k < 4; ++k)
            for (int i = 1; i <= 8; ++i) {
                const double t = i / 8.0;
                mouseMove(project(sq[k][0] + (sq[k + 1][0] - sq[k][0]) * t, sq[k][1] + (sq[k + 1][1] - sq[k][1]) * t, 10), Qt::LeftButton);
            }
        mouseRelease(project(3, -2, 10), Qt::LeftButton);
    }, [this] { return view_->cutChoosing(); });
    add("lasso: click outside the loop", [this] { click(project(8.5, 6, 10)); }, [this] {
        auto h = app_->history();
        return idle() && h.back().toMap()["source"].toString().startsWith("lasso_cut(\"CutBox\"") &&
               std::abs(objectValue("CutBox", "volume") - (2000 - 160)) < 2;  // 4 x 4 x 10 removed
    }, 30000);
    add("drill from the toolbox", [this] { clickItem("tool_drill"); }, [this] { return view_->cutKind() == "drill"; });
    add("drill: click on the top face", [this] {
        click(project(5, 7, 10));
    }, [this] {
        auto h = app_->history();
        // d = 2 mm through 10 mm: pi r^2 h = 31.4 mm^3 (48-gon: 31.33)
        return idle() && h.back().toMap()["source"].toString().startsWith("drill(\"CutBox\"") &&
               std::abs(objectValue("CutBox", "volume") - (1840 - 31.33)) < 1;
    }, 30000);
    add("front view for slicing", [this] { app_->requestView("front"); },
        [this] { return idle() && std::abs(view_->camForward().y() - 1.0f) < 1e-3f; });
    add("frame for slicing", [this] { view_->frameAll(); }, [this] { return idle(); });
    add("slice from the toolbox", [this] { clickItem("tool_slice"); }, [this] { return view_->cutKind() == "slice"; });
    add("slice: click a height on the front face", [this] {
        click(project(5, -10, 4));
        mouseMove(project(5, -10, 8));
    }, [this] { return view_->cutChoosing() && view_->cutHover() == 1; });
    add("slice: keep the part above", [this] { click(project(5, -10, 8)); }, [this] {
        auto h = app_->history();
        return idle() && h.back().toMap()["source"].toString().startsWith("slice_cut(\"CutBox\"") &&
               std::abs(objectValue("CutBox", "volume") - 0.6 * (1840 - 31.33)) < 1;
    }, 30000);
    add("leave the cut tool", [this] { key(Qt::Key_Escape); }, [this] { return view_->tool() == "select"; });
    add("iso view on the cut box", [this] { app_->requestView("iso"); view_->frameAll(); }, [this] { return idle(); });
    add("screenshot cut result", [this] { screenshot("17_cut_result"); });
    add("perspective again", [this] { app_->requestView("perspective"); }, [this] { return idle() && !view_->ortho(); });

    // 19) Moving: drag a selected box across a taller box that overlaps the path (top view).
    //     The move stays in the view plane: no jump onto the other box, exact target.
    add("move: two boxes", [this] {
        app_->setSelection({});
        for (const auto& o : app_->objects())
            if (o.toMap()["visible"].toBool()) app_->toggleVisible(o.toMap()["name"].toString());
        app_->execute("show(box(x=10*mm, y=10*mm, z=4*mm), name=\"MoveBox\")\n"
                      "show(box(x=6*mm, y=30*mm, z=12*mm).translate(x=14*mm), name=\"Wall\")", "ui");
    }, [this] { return idle() && hasObject("MoveBox") && hasObject("Wall"); }, 30000);
    add("move: ortho top view", [this] { app_->requestView("ortho"); app_->requestView("top"); },
        [this] { return idle() && view_->ortho() && std::abs(view_->camForward().z() + 1.0f) < 1e-3f; });
    add("move: frame", [this] { view_->frameAll(); }, [this] { return idle(); });
    add("move: select the box", [this] { app_->setSelection({"MoveBox"}); }, [this] { return view_->property("snapEnabled").toBool(); });
    add("move: drag across the wall", [this] {
        QPointF a = project(0, 0, 4);
        mouseMove(a);
        mousePress(a, Qt::LeftButton);
        for (int i = 1; i <= 20; ++i) mouseMove(project(28.0 * i / 20, 6.0 * i / 20, 4), Qt::LeftButton);  // over the wall
        mouseRelease(project(28, 6, 4), Qt::LeftButton);
    }, [this] {
        auto h = app_->history();
        return idle() && h.back().toMap()["source"].toString().startsWith("move(\"MoveBox\"") &&
               std::abs(objectValue("MoveBox", "cx") - 28) < 1e-9 && std::abs(objectValue("MoveBox", "cy") - 6) < 1e-9 &&
               std::abs(objectValue("MoveBox", "zmin")) < 1e-12;  // no jump in depth
    }, 30000);
    add("move: snap off", [this] { clickItem("snapToggle"); }, [this] { return !view_->property("snapEnabled").toBool(); });
    add("move: free drag back", [this] {
        QPointF a = project(28, 6, 4);
        mouseMove(a);
        mousePress(a, Qt::LeftButton);
        for (int i = 1; i <= 10; ++i) mouseMove(project(28 - 18.3 * i / 10, 6 - 3.7 * i / 10, 4), Qt::LeftButton);
        mouseRelease(project(9.7, 2.3, 4), Qt::LeftButton);
    }, [this] {
        auto h = app_->history();
        return idle() && h.back().toMap()["source"].toString().startsWith("move(\"MoveBox\"") &&
               std::abs(objectValue("MoveBox", "cx") - 9.7) < 0.1 && std::abs(objectValue("MoveBox", "zmin")) < 1e-12;
    }, 30000);
    add("move: snap on again", [this] { clickItem("snapToggle"); }, [this] { return view_->property("snapEnabled").toBool(); });
    add("move: perspective again", [this] { app_->requestView("perspective"); }, [this] { return idle() && !view_->ortho(); });

    // 20) Local AI assistant against a stand-in Ollama server (ANVIL_OLLAMA_URL, set by the test runner)
    if (qEnvironmentVariableIsSet("ANVIL_TEST_AI")) {
        add("AI mode: server found", [this] { app_->setMode("AI"); app_->aiRefresh(); },
            [this] { return app_->aiStatus().value("running").toBool(); }, 20000);
        add("AI installer: install Qwen3 8B", [this] { clickItem("aiInstall_qwen3:8b"); }, [this] {
            for (const auto& m : app_->aiStatus().value("models").toList())
                if (m.toMap()["name"].toString() == "qwen3:8b") return app_->aiModel() == "qwen3:8b" && !app_->aiBusy();
            return false;
        }, 20000);
        add("AI: use Qwen2.5 7B", [this] { app_->setAiModel("qwen2.5:7b"); }, [this] { return app_->aiModel() == "qwen2.5:7b"; });
        add("AI: ask from the panel", [this] { typeInto("aiPrompt", "Hebe den rechten Arm und gib ihm ein Schwert"); }, [this] {
            const auto p = app_->aiProposal();
            const QString py = p.value("python").toString();
            return !app_->aiBusy() && py.contains("edit('hero'") && py.contains("equip('hero', 'zweihander'");
        }, 30000);
        add("screenshot AI proposal", [this] { screenshot("18_ai_proposal"); });
        add("AI: apply = one command", [this] {
            hist_before_ = app_->history().size();
            clickItem("aiApply");
        }, [this] {
            auto h = app_->history();
            return idle() && h.size() == hist_before_ + 1 && h.back().toMap()["source"].toString().startsWith("edit('hero'") &&
                   h.back().toMap()["source"].toString().contains("equip(") && hasObject("Zweihander2");
        }, 30000);
        add("AI: invalid tool calls are refused", [this] { app_->aiAsk("bogus request"); }, [this] {
            const auto p = app_->aiProposal();
            return !app_->aiBusy() && p.value("python").toString().isEmpty() && p.value("errors").toList().size() == 2;
        }, 30000);
        add("AI: discard", [this] { clickItem("aiDiscard"); }, [this] { return app_->aiProposal().isEmpty(); });
        add("AI: back to Design", [this] { app_->setMode("Design"); }, [this] { return true; });
    }

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
