#include "app_controller.hpp"

#include <QDateTime>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <cmath>
#include <fstream>
#include <numbers>
#include <thread>

#include "anvil/expr.hpp"
#include "anvil/fsutil.hpp"
#include "anvil/journal.hpp"
#include "anvil/tolerance.hpp"
#include "anvil/sketch.hpp"
#include "anvil/version.hpp"
#include "viewport_item.hpp"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>  // K32GetProcessMemoryInfo (kernel32, PSAPI_VERSION 2)
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace anvil {

namespace {
constexpr int kConsoleMaxChars = 200000;

QString pyStr(const QString& s) {
    QString o = "\"";
    for (QChar c : s) {
        if (c == '\\' || c == '"') o += '\\';
        if (c == '\n') {
            o += "\\n";
            continue;
        }
        o += c;
    }
    return o + "\"";
}

QString reprDouble(double v) { return QString::fromStdString(repr_double(v)); }

bool pidAlive(qint64 pid) {
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!h) return false;
    bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
#else
    return ::kill(pid_t(pid), 0) == 0;
#endif
}

qint64 currentPid() {
#ifdef _WIN32
    return qint64(GetCurrentProcessId());
#else
    return qint64(::getpid());
#endif
}
}  // namespace

AppController::AppController(SessionOptions opt, std::filesystem::path session_dir, QObject* parent)
    : QObject(parent), opt_(std::move(opt)), session_dir_(std::move(session_dir)) {
    settings_ = Settings::load();
    if (auto u = parse_length_unit(settings_.unit.toStdString())) unit_ = *u;
    theme_ = settings_.theme;
    display_ = settings_.display;
    scenery_ = settings_.scenery;
    opt_.display_chord_mm = settings_.chord_mm;
    std::filesystem::create_directories(session_dir_);
    {
        std::ofstream lock(session_dir_ / "lock");
        lock << currentPid();
    }
    findRecovery();  // before our own journal exists in the sessions folder scan
    opt_.journal_path = session_dir_ / "journal.py";
    session_ = std::make_unique<Session>(opt_);
    session_->document().set_undo_limit(std::size_t(std::max(10, settings_.undo_limit)));
    session_->ui_handler = [this](const std::string& json) {
        QString j = QString::fromStdString(json);
        QMetaObject::invokeMethod(this, [this, j] { handleUiRequest(j); }, Qt::QueuedConnection);
    };
    session_->document().add_listener([this](const DocState&) {
        QMetaObject::invokeMethod(this, [this] { onDocumentChanged(); }, Qt::QueuedConnection);
    });
    builder_ = std::make_unique<SceneBuilder>(
        session_->cache(),
        [this](std::shared_ptr<const Scene> s) {
            QMetaObject::invokeMethod(this, [this, s] {
                if (viewport_) viewport_->setScene(s);
                // merge evaluation results into the object list
                QVariantList objs;
                qulonglong tris = 0;
                for (const auto& o : s->objects) {
                    QVariantMap m;
                    m["name"] = QString::fromStdString(o.name);
                    m["visible"] = o.visible;
                    m["triangles"] = qulonglong(o.triangles);
                    m["volume"] = o.volume;
                    m["manifold"] = o.manifold;
                    if (!o.bounds.empty()) {  // box centre and bottom (tests, readouts)
                        const Vec3 c = o.bounds.center();
                        m["cx"] = c.x, m["cy"] = c.y, m["cz"] = c.z, m["zmin"] = o.bounds.lo.z;
                    }
                    m["error"] = QString::fromStdString(o.error);
                    if (o.visible) tris += o.triangles;
                    objs << m;
                }
                // keep kind information from the document
                auto st = session_->document().state();
                for (int i = 0; i < objs.size(); ++i) {
                    QVariantMap m = objs[i].toMap();
                    if (const Object* ob = st->find(m["name"].toString().toStdString()))
                        m["kind"] = QString(node_kind_name(ob->root->kind));
                    objs[i] = m;
                }
                objects_ = objs;
                scene_tris_ = tris;
                emit documentChanged();
            }, Qt::QueuedConnection);
        },
        [this](std::shared_ptr<const MeshCpu> g) {
            QMetaObject::invokeMethod(this, [this, g] {
                if (viewport_) viewport_->setGhost(g);
            }, Qt::QueuedConnection);
        });
    loadCommands();
    radial_layout_ = settings_.radial_layout;
    connect(&autosave_timer_, &QTimer::timeout, this, &AppController::autosave);
    {
        auto* mt = new QTimer(this);  // status bar memory readout
        connect(mt, &QTimer::timeout, this, &AppController::memoryChanged);
        mt->start(2000);
    }
    autosave_timer_.start(std::max(5, settings_.autosave_seconds) * 1000);
    detectHardware();
    refreshModels();
}

AppController::~AppController() {
    builder_.reset();
    if (session_) {
        session_->close_clean();
        session_.reset();  // joins the Python thread after the close marker is written
    }
    // A cleanly closed session needs no recovery: remove its folder.
    std::error_code ec;
    std::filesystem::remove_all(session_dir_, ec);
    settings_.save();
}

void AppController::attachViewport(ViewportItem* v) {
    viewport_ = v;
    v->setRadialProvider([this](const std::vector<int>& path) {
        QVariantList p;
        for (int i : path) p << i;
        std::vector<RadialItem> ring;
        for (const auto& it : radialItems(p, viewport_->radialContext())) {
            QVariantMap m = it.toMap();
            ring.push_back({m["enabled"].toBool(), m["submenu"].toBool()});
        }
        return ring;
    });
    connect(v, &ViewportItem::picked, this, &AppController::pick);
    v->setContextResolver([this](const QString& c) {
        if (mode_ == "Design" && c == "selection" && selection_.size() == 1 && objectKind(selection_.front()) == "Sketch")
            return QString("sketch");
        return c;
    });
    connect(v, &ViewportItem::sketchDrawn, this, &AppController::onSketchDrawn);
    // cut tools: one command per cut (all targets in it, one undo step)
    connect(v, &ViewportItem::cutRequested, this,
            [this](const QStringList& targets, const QVariantList& path, const QVector3D& dir, const QString& mode,
                   const QString& keep, const QVariantList& eye) {
                auto num = [](double x, double q) {  // tidy script numbers: round to q, divide by an exact power of ten
                    const double inv = std::round(1.0 / q);
                    return reprDouble(std::round(x * inv) / inv);
                };
                auto vec = [&](const QVariantList& c, double q) {
                    return "(" + num(c[0].toDouble(), q) + ", " + num(c[1].toDouble(), q) + ", " + num(c[2].toDouble(), q) + ")";
                };
                const QVariantList d{dir.x(), dir.y(), dir.z()};
                QStringList lines;
                for (const auto& t : targets) {
                    if (mode == "plane") {
                        lines << "slice_cut(" + pyStr(t) + ", " + vec(path.front().toList(), 1e-4) + ", normal=" + vec(d, 1e-6) +
                                     ", keep=" + pyStr(keep) + ")";
                        continue;
                    }
                    QStringList pts;
                    for (const auto& p : path) pts << vec(p.toList(), 1e-4);  // 0.1 um
                    lines << QString(mode == "lasso" ? "lasso_cut(" : "knife(") + pyStr(t) + ", [" + pts.join(", ") +
                                 "], direction=" + vec(d, 1e-6) + ", keep=" + pyStr(keep) +
                                 (eye.size() == 3 ? ", eye=" + vec(eye, 1e-4) : QString()) + ")";
                }
                if (!lines.isEmpty()) execute(lines.join("\n"), "ui");
            });
    connect(v, &ViewportItem::drillRequested, this,
            [this](const QString& target, const QVector3D& p, const QVector3D& dir, double dmm) {
                auto num = [](double x, double q) {
                    const double inv = std::round(1.0 / q);
                    return reprDouble(std::round(x * inv) / inv);
                };
                execute("drill(" + pyStr(target) + ", (" + num(p.x(), 1e-4) + ", " + num(p.y(), 1e-4) + ", " + num(p.z(), 1e-4) +
                            "), d=" + reprDouble(dmm) + "*mm, direction=(" + num(dir.x(), 1e-6) + ", " + num(dir.y(), 1e-6) +
                            ", " + num(dir.z(), 1e-6) + "))",
                        "ui");
            });
    // a finished live stroke becomes one sculpt(...) command; its grid is
    // registered first, so the evaluation reuses it instead of re-sculpting
    v->setStrokeDone([this](const QString& object, NodePtr root, std::shared_ptr<const sdf::Grid> grid, sdf::Stroke st) {
        if (!root || root->kind != NodeKind::Sculpt || st.path.empty()) return;
        auto strokes = root->sculpt->strokes;
        strokes.push_back(st);
        auto doc = session_->document().state();
        try {
            seed_sculpt_cache(make_sculpt(root->inputs[0], root->sculpt->voxel, strokes), doc->resolver(),
                              doc->eval_settings(settings_.chord_mm), grid);
        } catch (const std::exception&) {
        }
        QStringList pts;
        for (const auto& p : st.path) pts << "(" + reprDouble(p.x) + ", " + reprDouble(p.y) + ", " + reprDouble(p.z) + ")";
        QString extra;
        if (st.opt.invert) extra += ", invert=True";
        if (st.opt.symmetry >= 0)
            extra += QString(", symmetry=\"") + "xyz"[st.opt.symmetry] + "\", sym_at=" + reprDouble(st.opt.sym_at);
        if (st.opt.falloff != 1.0) extra += ", falloff=" + reprDouble(st.opt.falloff);
        if (st.brush == sdf::Brush::Texture)
            extra += QString(", pattern=\"") + sdf::pattern_name(st.opt.pattern) + "\", pattern_size=" +
                     reprDouble(st.opt.pattern_size) + "*mm";
        execute("sculpt(" + pyStr(object) + ", " + pyStr(sdf::brush_name(st.brush)) + ", [" + pts.join(", ") +
                    "], radius=" + reprDouble(st.radius) + "*mm, strength=" + reprDouble(st.strength) + extra + ")",
                "ui");
    });
    connect(v, &ViewportItem::sculptConvertRequested, this, [this](const QString& name) {
        setSelection({name});
        execute("sculptable(" + pyStr(name) + ")", "ui");
    });
    connect(v, &ViewportItem::toolsPicked, this, &AppController::onToolsPicked);
    connect(v, &ViewportItem::objectDoubleClicked, this, [this](const QString& name) {
        setSelection({name});
        if (objectKind(name) == "Sketch") emit uiToolRequested("sketch_edit");
    });
    connect(v, &ViewportItem::radialSelected, this, &AppController::radialPick);
    connect(v, &ViewportItem::moveRequested, this, [this](const QStringList& names, double dx, double dy, double dz) {
        QStringList a;
        if (dx != 0) a << "x=" + reprDouble(dx) + "*mm";
        if (dy != 0) a << "y=" + reprDouble(dy) + "*mm";
        if (dz != 0) a << "z=" + reprDouble(dz) + "*mm";
        if (a.isEmpty() || names.isEmpty()) return;
        QStringList lines;  // all selected objects: one command, one undo step
        for (const auto& n : names) lines << "move(" + pyStr(n) + ", " + a.join(", ") + ")";
        execute(lines.join("\n"), "ui");
    });
    connect(this, &AppController::viewRequested, v, [v](const QString& name, const QVector3D& d) {
        if (name.isEmpty()) v->viewDirection(d.x(), d.y(), d.z());
        else v->view(name);
    });
    builder_->request(session_->document().state(), session_->document().revision(), settings_.chord_mm);
}

QString AppController::version() const { return QString(version_string()); }

// ------------------------------------------------------------------ sketches
QString AppController::objectKind(const QString& name) const {
    const Object* o = session_->document().state()->find(name.toStdString());
    return o && o->root ? QString(node_kind_name(o->root->kind)) : QString();
}

QString AppController::objectAttr(const QString& name, const QString& key) const {
    const Object* o = session_->document().state()->find(name.toStdString());
    if (!o || !o->root) return {};
    auto it = o->root->attrs.find(key.toStdString());
    return it == o->root->attrs.end() ? QString() : QString::fromStdString(it->second);
}

QString AppController::uniqueName(const QString& base) const {
    auto st = session_->document().state();
    for (int i = 1;; ++i) {
        QString n = base + QString::number(i);
        if (!st->find(n.toStdString())) return n;
    }
}

double AppController::sculptVoxel(const QString& name) const {
    const Object* o = session_->document().state()->find(name.toStdString());
    return o && o->root && o->root->kind == NodeKind::Sculpt ? o->root->sculpt->voxel : 0.0;
}

QString AppController::sketchTarget(const QString& sketch) const {
    QString t = sketch_on_.value(sketch);
    if (t.isEmpty() || t == sketch) return {};
    const Object* o = session_->document().state()->find(t.toStdString());
    return o && o->visible ? t : QString();
}

void AppController::onSketchDrawn(const QString& kind, const QString& plane, double offset, const QVariantList& pts,
                                  bool closed, const QString& on) {
    auto pt = [](const QVariant& v) {
        QPointF p = v.toPointF();
        return "(" + reprDouble(p.x()) + ", " + reprDouble(p.y()) + ")";
    };
    QString name = uniqueName("Sketch");
    QString tail = ", plane=" + pyStr(plane) + (offset != 0 ? ", offset=" + reprDouble(offset) + "*mm" : QString()) +
                   ", name=" + pyStr(name) + ")";
    QString src;
    if (kind == "polyline") {
        QStringList l;
        for (const auto& v : pts) l << pt(v);
        src = "sketch_polyline([" + l.join(", ") + "]" + (closed ? ", closed=True" : "") + tail;
    } else if (kind == "rect" && pts.size() == 2) {
        QPointF a = pts[0].toPointF(), b = pts[1].toPointF();
        if (std::abs(a.x() - b.x()) < tol::kConfusionMm || std::abs(a.y() - b.y()) < tol::kConfusionMm) return;  // degenerate
        src = "sketch_rect(" + pt(pts[0]) + ", " + pt(pts[1]) + tail;
    } else if (kind == "circle" && pts.size() == 2) {
        QPointF c = pts[0].toPointF(), q = pts[1].toPointF();
        double r = std::hypot(q.x() - c.x(), q.y() - c.y());
        if (r < tol::kConfusionMm) return;
        src = "sketch_circle(" + pt(pts[0]) + ", r=" + reprDouble(r) + "*mm" + tail;
    }
    if (src.isEmpty()) return;
    if (!on.isEmpty()) sketch_on_[name] = on;
    select_after_ = name;
    execute(src, "ui");
}

void AppController::onToolsPicked(const QString& target, const QStringList& tools) {
    if (objectKind(target) == "Sketch") {  // 'Cut through' without a known target: cut each picked part
        QStringList lines;
        for (const auto& t : tools) {
            sketch_on_[target] = t;
            lines << "cut_sketch(" + pyStr(target) + ", " + pyStr(t) + ")";
        }
        execute(lines.join("\n"), "ui");
        return;
    }
    QStringList a{pyStr(target)};
    for (const auto& t : tools) a << pyStr(t);
    select_after_ = target;
    execute("cut_with(" + a.join(", ") + ")", "ui");
}

QVariantMap AppController::sketchInfo(const QString& name) const {
    QVariantMap out;
    auto st = session_->document().state();
    const Object* o = st->find(name.toStdString());
    if (!o || !o->root || o->root->kind != NodeKind::Sketch || !o->root->sketch) return out;
    auto [sk, status] = solve_sketch(*o->root->sketch, st->resolver());
    auto world = [&](double x, double y) {
        Vec3 w = sk.to_world(x, y);
        return QVector3D(float(w.x), float(w.y), float(w.z));
    };
    out["name"] = name;
    out["plane"] = QString(plane_name(sk.plane));
    out["offset"] = sk.offset;
    out["dof"] = status.dof;
    out["converged"] = status.converged;
    out["message"] = QString::fromStdString(status.message);
    QVariantList pts;
    for (const auto& [id, p] : sk.points) {
        QVariantMap m;
        m["id"] = QString::fromStdString(id);
        m["x"] = p.x, m["y"] = p.y;
        m["world"] = world(p.x, p.y);
        auto it = status.entity_state.find(id);
        m["state"] = it != status.entity_state.end() ? QString::fromStdString(it->second) : QString("free");
        pts << m;
    }
    out["points"] = pts;
    QVariantList curves;
    for (const auto& [id, c] : sk.curves) {
        QVariantMap m;
        m["id"] = QString::fromStdString(id);
        m["kind"] = c.kind == CurveKind::Line ? "line" : c.kind == CurveKind::Circle ? "circle" : "arc";
        QStringList ps;
        for (const auto& p : c.pts) ps << QString::fromStdString(p);
        m["points"] = ps;
        m["radius"] = c.radius;
        m["construction"] = c.construction;
        // label anchor: line midpoint, circle/arc: a point on the curve
        if (c.kind == CurveKind::Line && c.pts.size() == 2) {
            const auto &a = sk.points.at(c.pts[0]), &b = sk.points.at(c.pts[1]);
            m["anchor"] = world((a.x + b.x) / 2, (a.y + b.y) / 2);
            m["length"] = std::hypot(b.x - a.x, b.y - a.y);
        } else if (!c.pts.empty()) {
            const auto& ctr = sk.points.at(c.pts[0]);
            double r = c.radius;
            if (c.kind == CurveKind::Arc && c.pts.size() >= 2) {
                const auto& s0 = sk.points.at(c.pts[1]);
                r = std::hypot(s0.x - ctr.x, s0.y - ctr.y);
            }
            m["anchor"] = world(ctr.x + r * std::numbers::sqrt2 / 2, ctr.y + r * std::numbers::sqrt2 / 2);
        }
        curves << m;
    }
    out["curves"] = curves;
    QVariantList cons;
    for (const auto& [id, k] : sk.constraints) {
        QVariantMap m;
        m["id"] = QString::fromStdString(id);
        m["kind"] = QString(constraint_name(k.kind));
        QStringList rs;
        for (const auto& r : k.refs) rs << QString::fromStdString(r);
        m["refs"] = rs;
        m["hasValue"] = constraint_has_value(k.kind);
        bool angle = k.kind == ConstraintKind::LineAngle;
        m["text"] = !k.value.expr.empty() ? QString::fromStdString(k.value.expr)
                    : angle ? QString::number(k.value.value * 180 / std::numbers::pi, 'g', 6) + " deg"
                            : formatLength(k.value.value);
        auto it = status.constraint_state.find(id);
        m["state"] = it != status.constraint_state.end() ? QString::fromStdString(it->second) : QString("ok");
        cons << m;
    }
    out["constraints"] = cons;
    return out;
}

double AppController::parseLength(const QString& text) const {
    static const QRegularExpression re(R"(^\s*([-+]?\d*[.,]?\d+(?:[eE][-+]?\d+)?)\s*([a-z]*)\s*$)");
    auto m = re.match(text);
    if (!m.hasMatch()) return std::nan("");
    double v = m.captured(1).replace(',', '.').toDouble();
    QString u = m.captured(2);
    if (u.isEmpty()) return v * unit_info(unit_).mm_per_unit;
    if (u == "inch") u = "in";
    auto lu = parse_length_unit(u.toStdString());
    return lu ? v * unit_info(*lu).mm_per_unit : std::nan("");
}

void AppController::sketchDrag(const QString& sketch, const QString& point, double x, double y) {
    execute("sketch_move(" + pyStr(sketch) + ", " + pyStr(point) + ", " + reprDouble(x) + "*mm, " + reprDouble(y) +
                "*mm)",
            "ui");
}

namespace {
// "40" -> 40*<unit>, "40 mm" -> 40*mm, otherwise an expression string
QString valueLiteral(const QString& text, const QString& unit, bool angle) {
    QString t = text.trimmed();
    static const QRegularExpression num(R"(^([-+]?\d*\.?\d+(?:[eE][-+]?\d+)?)\s*([a-zA-Zµ°]*)$)");
    auto m = num.match(t);
    if (m.hasMatch()) {
        QString u = m.captured(2);
        if (u == "°") u = "deg";
        if (u.isEmpty()) u = angle ? "deg" : unit;
        if (u == "in") u = "inch";
        if (u == "µm") u = "um";
        return m.captured(1) + "*" + u;
    }
    return pyStr(t);
}
}  // namespace

void AppController::sketchSetDim(const QString& sketch, const QString& constraint, const QString& text) {
    if (text.trimmed().isEmpty()) return;
    bool angle = false;
    if (const Object* o = session_->document().state()->find(sketch.toStdString()))
        if (o->root && o->root->sketch) {
            auto it = o->root->sketch->constraints.find(constraint.toStdString());
            angle = it != o->root->sketch->constraints.end() && it->second.kind == ConstraintKind::LineAngle;
        }
    execute("sketch_set(" + pyStr(sketch) + ", " + pyStr(constraint) + ", " + valueLiteral(text, unit(), angle) + ")",
            "ui");
}

void AppController::sketchConstrain(const QString& sketch, const QString& kind, const QStringList& refs) {
    QStringList a{pyStr(sketch), pyStr(kind)};
    for (const auto& r : refs) a << pyStr(r);
    execute("sketch_constrain(" + a.join(", ") + ")", "ui");
}

void AppController::sketchDimension(const QString& sketch, const QString& kind, const QStringList& refs,
                                    const QString& text) {
    QStringList a{pyStr(sketch), pyStr(kind)};
    for (const auto& r : refs) a << pyStr(r);
    a << valueLiteral(text, unit(), kind == "angle");
    execute("sketch_dim(" + a.join(", ") + ")", "ui");
}

void AppController::sketchDelete(const QString& sketch, const QStringList& ids) {
    QStringList a{pyStr(sketch)};
    for (const auto& r : ids) a << pyStr(r);
    execute("sketch_delete(" + a.join(", ") + ")", "ui");
}

void AppController::setStartupMs(double ms) {
    startup_ms_ = ms;
    emit startupMeasured();
}

void AppController::setSelection(const QStringList& s) {
    if (s == selection_) return;
    selection_ = s;
    if (viewport_) viewport_->setSelection(s);
    emit selectionChanged();
}

void AppController::setMode(const QString& m) {
    if (m == mode_) return;
    mode_ = m;
    emit modeChanged();
}

void AppController::pick(const QString& name, bool additive) {
    QStringList s = additive ? selection_ : QStringList();
    if (!name.isEmpty()) {
        if (additive && s.contains(name)) s.removeAll(name);
        else if (!s.contains(name)) s << name;
    }
    setSelection(s);
}

// ------------------------------------------------------------------ document sync
void AppController::onDocumentChanged() {
    refreshModels();
    builder_->request(session_->document().state(), session_->document().revision(), settings_.chord_mm);
}

void AppController::refreshModels() {
    auto st = session_->document().state();
    QVariantList hist;
    for (const auto& h : session_->document().history()) {
        QVariantMap m;
        QString label = QString::fromStdString(h.label);
        m["source"] = label;
        m["label"] = label.section('\n', 0, 0).left(120);
        m["origin"] = QString::fromStdString(h.origin);
        hist << m;
    }
    history_ = hist;
    history_pos_ = int(session_->document().history_position());
    can_undo_ = session_->document().can_undo();
    can_redo_ = session_->document().can_redo();
    QVariantList vars;
    auto res = st->resolver();
    for (const auto& [name, p] : st->vars) {
        QVariantMap m;
        m["name"] = QString::fromStdString(name);
        m["expr"] = QString::fromStdString(p.expr);
        Dim d = st->var_dims.count(name) ? st->var_dims.at(name) : dims::kLength;
        double v = res(name)->value;
        m["display"] = d == dims::kLength ? formatLength(v)
                       : d == dims::kAngle ? QString::number(v * 180 / std::numbers::pi, 'g', 8) + " deg"
                                           : QString::number(v, 'g', 10);
        vars << m;
    }
    variables_ = vars;
    // drop selected names that no longer exist
    QStringList sel;
    for (const auto& n : selection_)
        if (st->find(n.toStdString())) sel << n;
    if (!select_after_.isEmpty() && st->find(select_after_.toStdString())) {
        sel = {select_after_};
        select_after_.clear();
    }
    setSelection(sel);
    // object list (geometry info arrives with the next scene)
    QVariantList objs;
    for (const auto& o : st->objects) {
        QVariantMap m;
        m["name"] = QString::fromStdString(o->name);
        m["visible"] = o->visible;
        m["kind"] = QString(node_kind_name(o->root->kind));
        for (const auto& old : objects_)
            if (old.toMap()["name"] == m["name"]) {
                for (const char* k : {"triangles", "volume", "manifold", "error"}) m[k] = old.toMap()[k];
            }
        objs << m;
    }
    objects_ = objs;
    emit documentChanged();
}

// ------------------------------------------------------------------ commands
void AppController::appendConsole(const QString& text) {
    if (text.isEmpty()) return;
    console_ += text;
    if (!console_.endsWith('\n')) console_ += '\n';
    if (console_.size() > kConsoleMaxChars) console_ = console_.right(kConsoleMaxChars);
    emit consoleChanged();
}

void AppController::execute(const QString& source, const QString& origin) {
    if (source.trimmed().isEmpty()) return;
    ++pending_;
    emit busyChanged();
    appendConsole(">>> " + QString(source).replace("\n", "\n... "));
    session_->submit_async(source.toStdString(), origin.toStdString(), ExecMode::Command,
                           [this, origin](ExecResult r) {
                               QMetaObject::invokeMethod(this, [this, r, origin] {
                                   --pending_;
                                   emit busyChanged();
                                   appendConsole(QString::fromStdString(r.output));
                                   last_error_ = r.ok ? QString() : QString::fromStdString(r.error);
                                   if (!r.ok) appendConsole("error: " + last_error_);
                                   emit lastErrorChanged();
                                   emit commandFinished(r.ok, last_error_, origin);
                               }, Qt::QueuedConnection);
                           });
}

void AppController::loadCommands() {
    ExecResult r = session_->run("commands", "ui", ExecMode::Query);
    if (!r.ok) {
        appendConsole("error: cannot load the command registry: " + QString::fromStdString(r.error));
        return;
    }
    QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(r.json)).object();
    commands_.clear();
    fuzzy_.clear();
    command_meta_.clear();
    for (const auto& c : o["commands"].toArray()) {
        QVariantMap m = c.toObject().toVariantMap();
        commands_ << m;
        command_meta_[m["id"].toString()] = m;
        fuzzy_.push_back({m["id"].toString().toStdString(),
                          (m["label"].toString() + " " + m["category"].toString() + " " + m["keywords"].toString())
                              .toStdString(),
                          0});
    }
    QVariantMap layout = o["radial"].toObject().toVariantMap();
    if (settings_.radial_layout.isEmpty()) radial_layout_ = layout;
    default_layout_ = layout;
    ExecResult pl = session_->run("plugins", "ui", ExecMode::Query);
    for (const auto& p : QJsonDocument::fromJson(QByteArray::fromStdString(pl.json)).array()) {
        QJsonObject po = p.toObject();
        appendConsole(QString("plugin %1 %2: %3")
                          .arg(po["name"].toString(), po["version"].toString(),
                               po["loaded"].toBool() ? "loaded" : "skipped (" + po["error"].toString() + ")"));
    }
    emit commandsChanged();
}

QString AppController::commandLabel(const QString& id) const {
    return command_meta_.contains(id) ? command_meta_[id]["label"].toString() : id;
}

void AppController::runCommand(const QString& id, const QVector3D& point, bool havePoint) {
    if (!command_meta_.contains(id)) {
        appendConsole("error: unknown command '" + id + "'");
        return;
    }
    const QVariantMap& meta = command_meta_[id];
    int n = selection_.size();
    if (n < meta["min"].toInt() || n > meta["max"].toInt()) {
        last_error_ = QString("'%1' needs %2 selected object(s)")
                          .arg(meta["label"].toString(),
                               meta["min"].toInt() == meta["max"].toInt()
                                   ? meta["min"].toString()
                                   : QString("at least %1").arg(meta["min"].toInt()));
        emit lastErrorChanged();
        return;
    }
    for (auto& e : fuzzy_)
        if (e.id == id.toStdString()) e.usage += 1;
    QString ui = meta["ui"].toString();
    if (ui == "move_tool") {
        if (viewport_) viewport_->startMoveTool();
        return;
    }
    if (ui.startsWith("draw_")) {
        if (viewport_) viewport_->startDraw(ui.mid(5));
        return;
    }
    if (ui == "pick_tools") {
        if (selection_.size() >= 2) {  // already chosen: first minus the rest
            QStringList a;
            for (const auto& s : selection_) a << pyStr(s);
            execute("cut_with(" + a.join(", ") + ")", "ui");
        } else if (viewport_) {
            viewport_->startPickTools(selection_.front());
        }
        return;
    }
    if (ui == "sketch_cut") {
        const QString& sk = selection_.front();
        QString target = sketchTarget(sk);
        if (!target.isEmpty()) execute("cut_sketch(" + pyStr(sk) + ", " + pyStr(target) + ")", "ui");
        else if (viewport_) viewport_->startPickTools(sk, true);  // click the part to cut
        return;
    }
    if (!ui.isEmpty()) {
        emit uiToolRequested(ui);
        return;
    }
    QJsonObject ctx;
    ctx["selection"] = QJsonArray::fromStringList(selection_);
    if (havePoint) {
        // QVector3D is float: take the exact double point captured by the viewport
        Vec3 p = viewport_ ? viewport_->radialPointExact() : Vec3{point.x(), point.y(), point.z()};
        ctx["point"] = QJsonArray{p.x, p.y, p.z};
    }
    QJsonObject req{{"q", "build"}, {"id", id}, {"ctx", ctx}};
    std::string q = QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString();
    session_->submit_async(q, "ui", ExecMode::Query, [this](ExecResult r) {
        QMetaObject::invokeMethod(this, [this, r] {
            if (!r.ok) {
                last_error_ = QString::fromStdString(r.error);
                emit lastErrorChanged();
                return;
            }
            QString src = QJsonDocument::fromJson(QByteArray::fromStdString(r.json)).object()["source"].toString();
            if (!src.isEmpty()) execute(src, "ui");
        }, Qt::QueuedConnection);
    });
}

QVariantList AppController::radialItems(const QVariantList& path, const QString& context) const {
    QVariantList ring = radial_layout_.value(mode_).toMap().value(context).toList();
    if (ring.isEmpty()) ring = default_layout_.value(mode_).toMap().value(context).toList();
    for (const auto& idx : path) {
        int i = idx.toInt();
        if (i < 0 || i >= ring.size()) return {};
        ring = ring[i].toMap().value("items").toList();
    }
    QVariantList out;
    int n = selection_.size();
    for (int i = 0; i < 8; ++i) {
        QVariantMap m;
        if (i < ring.size()) {
            QVariant slot = ring[i];
            if (slot.typeId() == QMetaType::QVariantMap) {
                QVariantMap sm = slot.toMap();
                m["label"] = sm["submenu"].toString() + " ▸";
                m["submenu"] = true;
                m["enabled"] = true;
                m["id"] = "";
            } else if (!slot.toString().isEmpty() && command_meta_.contains(slot.toString())) {
                const QVariantMap& meta = command_meta_[slot.toString()];
                m["id"] = slot.toString();
                m["label"] = meta["label"];
                m["help"] = meta["help"];
                m["submenu"] = false;
                m["enabled"] = n >= meta["min"].toInt() && n <= meta["max"].toInt();
            }
        }
        if (!m.contains("enabled")) m["enabled"] = false, m["submenu"] = false, m["label"] = "", m["id"] = "";
        out << m;
    }
    return out;
}

void AppController::radialPick(const QVariantList& path, const QString& context, const QVector3D& point) {
    if (path.isEmpty()) return;
    QVariantList parent = path.mid(0, path.size() - 1);
    QVariantList ring = radialItems(parent, context);
    int i = path.last().toInt();
    if (i < 0 || i >= ring.size()) return;
    QString id = ring[i].toMap()["id"].toString();
    if (!id.isEmpty()) runCommand(id, point, true);
}

void AppController::undo() { session_->undo(); }
void AppController::redo() { session_->redo(); }
void AppController::jumpToHistory(int position) {
    int delta = position - history_pos_;
    for (int i = 0; i < -delta; ++i) session_->undo();
    for (int i = 0; i < delta; ++i) session_->redo();
}

// ------------------------------------------------------------------ properties
double AppController::processMemory() const {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return double(pmc.WorkingSetSize);
    return 0;
#else
    QFile f("/proc/self/statm");  // pages: size resident ...
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const auto parts = QString::fromLatin1(f.readAll()).split(' ');
    return parts.size() > 1 ? parts[1].toDouble() * 4096.0 : 0;
#endif
}

QString AppController::formatLength(double mm) const { return QString::fromStdString(format_length(mm, unit_)); }

QString AppController::formatPoint(const QVector3D& p) const {
    const double k = unit_info(unit_).mm_per_unit;
    auto f = [&](float v) { return QString::number(double(v) / k, 'g', 6); };
    return QString("x %1  y %2  z %3 %4").arg(f(p.x()), f(p.y()), f(p.z()), unit_info(unit_).symbol);
}

void AppController::requestViewDirection(const QVector3D& d) {
    if (d.isNull()) return;
    execute(QString("view(direction=(%1, %2, %3))").arg(d.x()).arg(d.y()).arg(d.z()), "ui");
}

QVariantList AppController::nodeTree(const QString& name) const {
    QVariantList rows;
    auto st = session_->document().state();
    const Object* o = st->find(name.toStdString());
    if (!o) return rows;
    std::function<void(const NodePtr&, const QString&, int)> walk = [&](const NodePtr& n, const QString& path, int depth) {
        QVariantMap row;
        row["path"] = path;
        row["depth"] = depth;
        QString kind = node_kind_name(n->kind);
        if (n->kind == NodeKind::Boolean)
            kind = n->op == BoolOp::Union ? "Union" : n->op == BoolOp::Difference ? "Subtract" : "Intersect";
        if (n->kind == NodeKind::ImportedMesh) kind = "Mesh  " + QString::fromStdString(n->source_path);
        row["kind"] = kind;
        QVariantList params;
        for (const auto& spec : param_specs(n->kind)) {
            auto it = n->params.find(spec.name);
            if (it == n->params.end() && std::string(spec.name) != "d2") continue;
            QVariantMap p;
            p["key"] = QString(spec.name);
            p["help"] = QString(spec.help);
            p["dim"] = spec.dim == dims::kAngle ? "Angle" : spec.dim == dims::kLength ? "Length" : "None";
            if (it == n->params.end()) {
                p["text"] = "";
                p["expr"] = "";
            } else {
                double v = it->second.value;
                if (!it->second.expr.empty()) {
                    try {
                        v = resolve(it->second, st->resolver(), spec.dim, spec.name);
                    } catch (...) {
                    }
                }
                p["expr"] = QString::fromStdString(it->second.expr);
                p["text"] = !it->second.expr.empty() ? QString::fromStdString(it->second.expr)
                            : spec.dim == dims::kAngle   ? QString::number(v * 180 / std::numbers::pi, 'g', 10) + " deg"
                            : spec.dim == dims::kLength  ? formatLength(v)
                                                         : QString::number(v, 'g', 10);
                p["value"] = spec.dim == dims::kAngle ? QString::number(v * 180 / std::numbers::pi, 'g', 8) + " deg"
                             : spec.dim == dims::kLength ? formatLength(v)
                                                         : QString::number(v, 'g', 8);
                // numeric value in the field's unit (mm in the display unit, deg, or plain) for scrubbing
                p["number"] = spec.dim == dims::kAngle    ? v * 180 / std::numbers::pi
                              : spec.dim == dims::kLength ? v / unit_info(unit_).mm_per_unit
                                                          : v;
            }
            params << p;
        }
        row["params"] = params;
        rows << row;
        for (const auto& c : n->child_names()) walk(n->child(c), path.isEmpty() ? QString::fromStdString(c) : path + "/" + QString::fromStdString(c), depth + 1);
    };
    walk(o->root, "", 0);
    return rows;
}

QString AppController::paramLiteral(const QString& text, Dim dim, QString* error) const {
    std::string t = text.trimmed().toStdString();
    auto st = session_->document().state();
    try {
        auto ids = expr_identifiers(t);
        if (!ids.empty()) {  // parametric: keep the expression, check it resolves now
            RtQuantity q = eval_expr(t, st->resolver());
            if (!(q.dim == dim)) throw ExprError("expected " + dim_name(dim) + ", got " + dim_name(q.dim), 0);
            return pyStr(text.trimmed());
        }
        RtQuantity q = eval_expr(t);
        if (q.dim.dimensionless()) {  // bare number: the display unit applies
            if (dim == dims::kLength) return reprDouble(q.value * unit_info(unit_).mm_per_unit) + "*mm";
            if (dim == dims::kAngle) return reprDouble(q.value) + "*deg";
            return reprDouble(q.value);  // factors, counts
        }
        if (!(q.dim == dim)) throw ExprError("expected " + dim_name(dim) + ", got " + dim_name(q.dim), 0);
        // "<number> <unit>" stays readable in the generated command (30*deg, 1.5*inch)
        static const QRegularExpression simple(R"(^\s*([-+]?\d*\.?\d+(?:[eE][-+]?\d+)?)\s*\*?\s*(mm|cm|m|um|in|inch|deg|rad)\s*$)");
        auto m = simple.match(text);
        if (m.hasMatch()) {
            QString u = m.captured(2) == "in" ? "inch" : m.captured(2);
            return reprDouble(m.captured(1).toDouble()) + "*" + u;
        }
        if (dim == dims::kLength) return reprDouble(q.value) + "*mm";
        return reprDouble(q.value) + "*rad";
    } catch (const std::exception& e) {
        if (error) *error = QString::fromStdString(e.what());
        return {};
    }
}

QString AppController::validateField(const QString& text, const QString& dim) const {
    QString err;
    paramLiteral(text, dim == "Angle" ? dims::kAngle : dim == "None" ? Dim{} : dims::kLength, &err);
    return err;
}

Dim AppController::paramDim(const QString& name, const QString& path, const QString& key) const {
    const Object* o = session_->document().state()->find(name.toStdString());
    if (!o || !o->root) return dims::kLength;
    NodePtr n = o->root;
    try {
        for (const auto& part : path.split('/', Qt::SkipEmptyParts)) {
            n = n->child(part.toStdString());
            if (!n) return dims::kLength;
        }
    } catch (const std::exception&) {
        return dims::kLength;
    }
    for (const auto& spec : param_specs(n->kind))
        if (key == spec.name) return spec.dim;
    return dims::kLength;
}

void AppController::previewParam(const QString& name, const QString& path, const QString& key, const QString& text) {
    QString lit = paramLiteral(text, paramDim(name, path, key), nullptr);
    if (lit.isEmpty()) return;
    preview("edit_preview(" + pyStr(name) + ", " + pyStr(path) + ", " + key + "=" + lit + ")");
}

void AppController::setParam(const QString& name, const QString& path, const QString& key, const QString& text) {
    Dim dim = paramDim(name, path, key);
    QString err;
    QString lit = paramLiteral(text, dim, &err);
    if (lit.isEmpty()) {
        last_error_ = key + ": " + err;
        emit lastErrorChanged();
        return;
    }
    QString src = "edit(" + pyStr(name) + (path.isEmpty() ? "" : ", " + pyStr(path)) + ", " + key + "=" + lit + ")";
    execute(src, "ui");
}

void AppController::setVariable(const QString& name, const QString& text) {
    QString err;
    QString lit = paramLiteral(text, dims::kLength, &err);
    if (lit.isEmpty()) {
        last_error_ = name + ": " + err;
        emit lastErrorChanged();
        return;
    }
    execute("var(" + pyStr(name) + ", " + lit + ")", "ui");
}

void AppController::toggleVisible(const QString& name) {
    auto st = session_->document().state();
    const Object* o = st->find(name.toStdString());
    if (!o) return;
    execute(QString(o->visible ? "hide(" : "unhide(") + pyStr(name) + ")", "ui");
}

void AppController::renameObject(const QString& from, const QString& to) {
    if (to.trimmed().isEmpty() || from == to) return;
    execute("rename(" + pyStr(from) + ", " + pyStr(to.trimmed()) + ")", "ui");
}

// ------------------------------------------------------------------ command bar
QVariantList AppController::fuzzy(const QString& query) const {
    QVariantList out;
    for (std::size_t i : fuzzy_rank(query.toStdString(), fuzzy_, 12)) {
        QVariantMap m = command_meta_[QString::fromStdString(fuzzy_[i].id)];
        int n = selection_.size();
        m["enabled"] = n >= m["min"].toInt() && n <= m["max"].toInt();
        out << m;
    }
    return out;
}

QVariantMap AppController::checkSyntax(const QString& source) {
    ExecResult r = session_->run(source.toStdString(), "ui", ExecMode::Check);
    return {{"ok", r.ok}, {"error", QString::fromStdString(r.error)}, {"line", r.error_line}, {"col", r.error_col}};
}

QVariantList AppController::complete(const QString& source) {
    ExecResult r = session_->run(source.toStdString(), "ui", ExecMode::Complete);
    QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(r.json)).object();
    QVariantList l;
    for (const auto& v : o["items"].toArray()) l << v.toString();
    return l;
}

QString AppController::signature(const QString& source) {
    QString name = source.section('(', 0, 0).trimmed();
    name = name.section(QRegularExpression("[^A-Za-z0-9_.]"), -1);
    if (name.isEmpty()) return {};
    QJsonObject req{{"q", "signature"}, {"name", name}};
    ExecResult r = session_->run(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString(), "ui", ExecMode::Query);
    if (!r.ok) return {};
    QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(r.json)).object();
    return o["signature"].toString() + (o["doc"].toString().isEmpty() ? "" : "  -  " + o["doc"].toString());
}

void AppController::preview(const QString& source) {
    const std::uint64_t seq = ++preview_seq_;
    session_->submit_async(source.toStdString(), "preview", ExecMode::Preview, [this, seq](ExecResult r) {
        QMetaObject::invokeMethod(this, [this, r, seq] {
            if (seq != preview_seq_) return;  // a newer preview superseded this one
            if (!r.ok && !r.error.empty()) emit previewError(QString::fromStdString(r.error));
            else emit previewError({});
            builder_->request_ghost(r.preview, session_->document().state(), settings_.chord_mm);
        }, Qt::QueuedConnection);
    });
}

void AppController::clearPreview() {
    ++preview_seq_;
    builder_->request_ghost(nullptr, nullptr, settings_.chord_mm);
}

void AppController::requestUnit(const QString& s) { execute("set_unit(" + pyStr(s) + ")", "ui"); }
void AppController::requestTheme(const QString& n) { execute("theme(" + pyStr(n) + ")", "ui"); }
void AppController::requestDisplay(const QString& m) { execute("display(" + pyStr(m) + ")", "ui"); }
void AppController::requestScenery(const QString& n) { execute("scenery(" + pyStr(n) + ")", "ui"); }
void AppController::requestView(const QString& n) {
    if (n.startsWith("dir:")) {
        QStringList c = n.mid(4).split(',');
        if (c.size() == 3) execute(QString("view(direction=(%1, %2, %3))").arg(c[0], c[1], c[2]), "ui");
        return;
    }
    execute("view(" + pyStr(n) + ")", "ui");
}

void AppController::handleUiRequest(const QString& json) {
    QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
    QString req = o["request"].toString();
    if (req == "view") {
        if (o.contains("direction")) {
            QJsonArray d = o["direction"].toArray();
            emit viewRequested({}, QVector3D(float(d[0].toDouble()), float(d[1].toDouble()), float(d[2].toDouble())));
        } else {
            emit viewRequested(o["name"].toString(), {});
        }
    } else if (req == "select") {
        QStringList s;
        for (const auto& v : o["names"].toArray()) s << v.toString();
        setSelection(s);
    } else if (req == "unit") {
        if (auto u = parse_length_unit(o["unit"].toString().toStdString())) {
            unit_ = *u;
            settings_.unit = unit_info(unit_).symbol;
            settings_.save();
            refreshModels();
            emit unitChanged();
        }
    } else if (req == "display") {
        display_ = o["mode"].toString();
        settings_.display = display_;
        settings_.save();
        emit displayChanged();
    } else if (req == "scenery") {
        scenery_ = o["name"].toString();
        settings_.scenery = scenery_;
        settings_.save();
        emit sceneryChanged();
    } else if (req == "theme") {
        theme_ = o["name"].toString();
        settings_.theme = theme_;
        settings_.save();
        emit themeChanged();
    }
}

// ------------------------------------------------------------------ files
QString AppController::selectionExpr(const QStringList& names) const {
    QStringList n = names;
    if (n.isEmpty()) {
        for (const auto& o : session_->document().state()->objects)
            if (o->visible) n << QString::fromStdString(o->name);
    }
    if (n.size() == 1) return pyStr(n.front());
    QStringList parts;
    for (const auto& x : n) parts << "get(" + pyStr(x) + ")";
    return parts.join(" | ");
}

QVariantMap AppController::checkForExport(const QStringList& names, double chordMm) {
    QVariantMap out;
    if (session_->document().state()->objects.empty()) {
        out["ok"] = false;
        out["summary"] = "Nothing to export: the document is empty.";
        return out;
    }
    QJsonObject req{{"q", "check"}, {"expr", selectionExpr(names)}, {"chord", chordMm}};
    ExecResult r = session_->run(QJsonDocument(req).toJson(QJsonDocument::Compact).toStdString(), "ui", ExecMode::Query);
    if (!r.ok) {
        out["ok"] = false;
        out["summary"] = QString::fromStdString(r.error);
        return out;
    }
    return QJsonDocument::fromJson(QByteArray::fromStdString(r.json)).object().toVariantMap();
}

void AppController::exportStl(const QStringList& names, const QString& path, const QString& unit, double chordMm,
                              bool ascii) {
    QString u = unit == "in" ? "inch" : unit;
    QString src = "export_stl(" + selectionExpr(names) + ", " + pyStr(QDir::toNativeSeparators(path)) + ", unit=" + u +
                  ", chord=" + reprDouble(chordMm) + "*mm" + (ascii ? ", ascii=True" : "") + ")";
    execute(src, "ui");
}

QString AppController::defaultExportPath() const {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString base = selection_.isEmpty() ? "export" : selection_.front();
    return QDir(dir).filePath(base + ".stl");
}

QString AppController::dropFiles(const QList<QUrl>& urls) {
    QStringList notes;
    for (const auto& u : urls) {
        QString p = u.toLocalFile();
        QString ext = QFileInfo(p).suffix().toLower();
        if (ext == "stl") execute("import_stl(" + pyStr(QDir::toNativeSeparators(p)) + ")", "ui");
        else if (ext == "anvil") openDocument(p);
        else if (ext == "step" || ext == "stp") execute("import_step(" + pyStr(QDir::toNativeSeparators(p)) + ")", "ui");
        else if (ext == "glb" || ext == "gltf" || ext == "obj")  // minis from Tripo, Meshy, Blender: posable
            execute("import_model(" + pyStr(QDir::toNativeSeparators(p)) + ", height=32*mm)", "ui");
        else if (ext == "fbx") notes << QFileInfo(p).fileName() + ": FBX is not supported; export the model as GLB (Tripo, Meshy and Blender can)";
        else if (ext == "3mf") notes << QFileInfo(p).fileName() + ": 3MF import arrives in M5";
        else if (ext == "png") notes << QFileInfo(p).fileName() + ": image input arrives with the AI tools (M6)";
        else if (ext == "keras") notes << QFileInfo(p).fileName() + ": Keras models arrive with the Neural Lab (M6)";
        else notes << QFileInfo(p).fileName() + ": unsupported file type";
    }
    if (!notes.isEmpty()) {
        last_error_ = notes.join("\n");
        emit lastErrorChanged();
    }
    return notes.join("\n");
}

void AppController::saveDocument(const QString& path) {
    QString p = path.endsWith(".anvil") ? path : path + ".anvil";
    execute("save(" + pyStr(QDir::toNativeSeparators(p)) + ")", "ui");
    doc_path_ = p;
    settings_.recent.removeAll(p);
    settings_.recent.prepend(p);
    emit documentPathChanged();
}

void AppController::openDocument(const QString& path) {
    execute("load(" + pyStr(QDir::toNativeSeparators(path)) + ")", "ui");
    doc_path_ = path;
    emit documentPathChanged();
}

// ------------------------------------------------------------------ autosave / recovery
void AppController::autosave() {
    auto rev = session_->document().revision();
    if (rev == saved_revision_) return;
    try {
        session_->save(session_dir_ / "autosave.anvil");
        saved_revision_ = rev;
        autosave_info_ = "Autosaved " + QDateTime::currentDateTime().toString("HH:mm:ss");
    } catch (const std::exception& e) {
        autosave_info_ = QString("Autosave failed: ") + e.what();
    }
    emit autosaveChanged();
}

void AppController::findRecovery() {
    QDir root(QString::fromStdString(session_dir_.parent_path().string()));
    QFileInfo best;
    for (const QFileInfo& d : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time)) {
        if (d.absoluteFilePath() == QString::fromStdString(session_dir_.string())) continue;
        QFile lock(d.absoluteFilePath() + "/lock");
        if (lock.open(QIODevice::ReadOnly) && pidAlive(lock.readAll().trimmed().toLongLong())) continue;  // other instance
        QString j = d.absoluteFilePath() + "/journal.py";
        if (!QFileInfo::exists(j)) {
            QDir(d.absoluteFilePath()).removeRecursively();
            continue;
        }
        JournalContents c = Journal::read(j.toStdString());
        int n = 0;
        for (const auto& a : c.actions)
            if (a.kind == JournalAction::Txn && a.ok) ++n;
        if (c.clean_close || n == 0) {
            QDir(d.absoluteFilePath()).removeRecursively();
            continue;
        }
        if (!best.exists()) {
            best = d;
            recovery_journal_ = j;
            recovery_info_ = QString("An earlier session ended unexpectedly (%1, %2 command(s)). Restore it?")
                                 .arg(d.lastModified().toString("yyyy-MM-dd HH:mm"))
                                 .arg(n);
        }
    }
}

void AppController::recover() {
    QString j = recovery_journal_;
    if (j.isEmpty()) return;
    std::thread([this, j] {
        RecoveryReport rep = session_->replay(j.toStdString());
        QMetaObject::invokeMethod(this, [this, rep, j] {
            appendConsole(QString("recovered %1 command(s)").arg(rep.replayed));
            for (const auto& u : rep.unfinished)
                appendConsole("not replayed (it was running when the session ended): " + QString::fromStdString(u));
            for (const auto& e : rep.errors) appendConsole("recovery error: " + QString::fromStdString(e));
            QDir(QFileInfo(j).absolutePath()).removeRecursively();
            recovery_journal_.clear();
            recovery_info_.clear();
            emit recoveryChanged();
        }, Qt::QueuedConnection);
    }).detach();
}

void AppController::discardRecovery() {
    if (!recovery_journal_.isEmpty()) QDir(QFileInfo(recovery_journal_).absolutePath()).removeRecursively();
    recovery_journal_.clear();
    recovery_info_.clear();
    emit recoveryChanged();
}

void AppController::saveRadialLayout(const QVariantMap& layout) {
    radial_layout_ = layout;
    settings_.radial_layout = layout;
    settings_.save();
}

void AppController::resetRadialLayout() {
    settings_.radial_layout.clear();
    settings_.save();
    radial_layout_ = default_layout_;
}

// ------------------------------------------------------------------ hardware
void AppController::detectHardware() {
    auto* p = new QProcess(this);
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus) {
        QString out = QString::fromUtf8(p->readAllStandardOutput()).trimmed();
        if (code == 0 && !out.isEmpty()) {
            QStringList f = out.section('\n', 0, 0).split(',');
            for (auto& s : f) s = s.trimmed();
            gpu_info_ = f.join("  ·  ");
            if (f.size() >= 2) {
                double mib = f[1].section(' ', 0, 0).toDouble();
                if (mib > 0 && mib < 12 * 1024 * 0.95)
                    hw_warning_ = QString("GPU has %1 GiB VRAM; ANVIL is tuned for 12 GiB").arg(mib / 1024, 0, 'f', 1);
            }
        } else {
            gpu_info_ = "nvidia-smi not available";
        }
        emit gpuInfoChanged();
        p->deleteLater();
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError) {
        gpu_info_ = "nvidia-smi not available";
        emit gpuInfoChanged();
        p->deleteLater();
    });
    p->start("nvidia-smi", {"--query-gpu=name,memory.total,driver_version,compute_cap", "--format=csv,noheader"});
}

}  // namespace anvil
