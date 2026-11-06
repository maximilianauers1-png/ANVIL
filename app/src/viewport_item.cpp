#include "viewport_item.hpp"
#include "sculpt_engine.hpp"

#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>
#include <QRunnable>
#include <QSGSimpleTextureNode>
#include <QtQuick/qsgtexture_platform.h>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace anvil {

namespace {
constexpr double kOrbitRadPerPx = 0.008;
constexpr double kDragThresholdPx = 4.0;
constexpr double kWheelStep = 0.85;     // zoom factor per wheel notch
constexpr double kAnimMs = 180.0;       // camera transition
constexpr double kCubeScale = 0.55;     // ViewCube: projected half-size of the unit cube (QML uses the same)
constexpr std::size_t kFrameWindow = 600;

class CleanupJob : public QRunnable {
  public:
    explicit CleanupJob(ViewportRenderer* r) : r_(r) {}
    void run() override { delete r_; }

  private:
    ViewportRenderer* r_;
};

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    double idx = p * double(v.size() - 1);
    std::size_t lo = std::size_t(std::floor(idx)), hi = std::min(v.size() - 1, lo + 1);
    return v[lo] + (v[hi] - v[lo]) * (idx - double(lo));
}
}  // namespace

ViewportItem::ViewportItem(QQuickItem* parent) : QQuickItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    setAcceptHoverEvents(true);
    setFlag(ItemHasContents, true);
    setFlag(ItemAcceptsInputMethod, false);
    connect(this, &QQuickItem::windowChanged, this, &ViewportItem::handleWindowChanged);
    connect(this, &ViewportItem::styleChanged, this, &ViewportItem::requestFrame);
    frame_ms_.assign(kFrameWindow, -1.0);
    // status bar values (GPU memory, triangles, device) refresh even when idle
    auto* t = new QTimer(this);
    connect(t, &QTimer::timeout, this, &ViewportItem::statsChanged);
    t->start(1000);
    snap_engine_.settings.dpi_scale = 1.0;
    // live sculpting: the engine thread posts results to the GUI thread
    sculpt_engine_ = std::make_unique<SculptEngine>(
        [this](const std::string& object, std::vector<SculptEngine::ChunkUpdate> ups, double ms) {
            QMetaObject::invokeMethod(this, [this, object, ups = std::move(ups), ms] {
                if (live_.object != object) return;
                for (const auto& u : ups) live_.chunks[sdf_chunk_key(u.chunk)] = u.mesh;
                sculpt_ms_ = ms;
                emit brushChanged();
                requestFrame();
            }, Qt::QueuedConnection);
        },
        [this](const std::string& object, std::shared_ptr<const sdf::Grid> grid, sdf::Stroke stroke) {
            QMetaObject::invokeMethod(this, [this, object, grid, stroke] {
                live_.done = true;
                pending_grid_ = grid;
                // the root the stroke's command will produce (chained strokes build on it)
                if (stroke_root_ && stroke_root_->kind == NodeKind::Sculpt) {
                    auto strokes = stroke_root_->sculpt->strokes;
                    strokes.push_back(stroke);
                    expected_root_ = make_sculpt(stroke_root_->inputs[0], stroke_root_->sculpt->voxel, strokes);
                }
                if (stroke_done_) stroke_done_(QString::fromStdString(object), stroke_root_, grid, stroke);
            }, Qt::QueuedConnection);
        });
}

ViewportItem::~ViewportItem() = default;

double ViewportItem::dpr() const { return window() ? window()->effectiveDevicePixelRatio() : 1.0; }

void ViewportItem::handleWindowChanged(QQuickWindow* w) {
    if (!w) return;
    connect(w, &QQuickWindow::beforeSynchronizing, this, &ViewportItem::sync, Qt::DirectConnection);
    connect(w, &QQuickWindow::sceneGraphInvalidated, this, &ViewportItem::cleanup, Qt::DirectConnection);
    connect(w, &QQuickWindow::frameSwapped, this, &ViewportItem::frameSwapped, Qt::QueuedConnection);
    snap_engine_.settings.dpi_scale = w->effectiveDevicePixelRatio();
}

void ViewportItem::sync() {
    // Render thread; the GUI thread is blocked, so reading members is safe.
    if (!renderer_) {
        renderer_ = new ViewportRenderer(window(), stats_);
        auto* r = renderer_;
        // A renderer error must never terminate the process silently: log it and stop drawing.
        connect(window(), &QQuickWindow::beforeRendering, window(), [r] {
            try {
                r->before_rendering();
            } catch (const std::exception& e) {
                qCritical("viewport renderer: %s", e.what());
            }
        }, Qt::DirectConnection);

    }
    FrameInput in;
    in.cam = cam_;
    QRectF r = mapRectToScene(boundingRect());
    const double k = dpr();
    in.rect_px = QRectF(std::round(r.x() * k), std::round(r.y() * k), std::round(r.width() * k), std::round(r.height() * k));
    in.cam.width = in.rect_px.width();
    in.cam.height = in.rect_px.height();
    in.bg[0] = float(bg_.redF()), in.bg[1] = float(bg_.greenF()), in.bg[2] = float(bg_.blueF());
    in.grid[0] = float(grid_.redF()), in.grid[1] = float(grid_.greenF()), in.grid[2] = float(grid_.blueF());
    in.edge[0] = float(edge_.redF()), in.edge[1] = float(edge_.greenF()), in.edge[2] = float(edge_.blueF());
    in.edge[3] = float(edge_.alphaF());
    in.show_grid = show_grid_;
    in.mesh_view = mesh_view_;
    // Scenery: light direction per scene (sun / moon), elevation e and azimuth a
    // (from +X towards +Y); exposure compensates the different illuminances.
    {
        // exposure and haze tuned against the scattering model (bench/scenery_tune.py)
        struct SceneryDef { const char* name; int id; double el_deg, az_deg; float exposure, haze; };
        static const SceneryDef scenes[] = {{"sunset", 1, 2.0, 200, 12.0f, 2.0f},
                                       {"fjord", 2, 24, 20, 8.0f, 1.5f},
                                       {"night", 3, 32, 150, 0.9f, 1.0f}};
        in.scenery = 0;
        for (const auto& sc : scenes)
            if (scenery_ == sc.name) {
                const double e = sc.el_deg * std::numbers::pi / 180, a = sc.az_deg * std::numbers::pi / 180;
                in.scenery = sc.id;
                in.sun[0] = float(std::cos(e) * std::cos(a)), in.sun[1] = float(std::cos(e) * std::sin(a));
                in.sun[2] = float(std::sin(e));
                in.exposure = sc.exposure;
                in.haze = sc.haze;
                in.grid_opacity *= 0.55f;  // the grid stays readable without dominating the scenery
            }
    }
    in.wire[0] = float(wire_.redF()), in.wire[1] = float(wire_.greenF()), in.wire[2] = float(wire_.blueF());
    in.wire[3] = float(wire_.alphaF());
    // adaptive grid: minor lines at least ~10 px apart, steps of 10
    double wpp = cam_.world_per_pixel(cam_.distance) * k / std::max(k, 1.0);
    double step = 0.1;
    while (step / std::max(wpp, 1e-12) < 10) step *= 10;
    in.grid_step = step;
    if (scene_) {
        int idx = 0;
        for (const auto& o : scene_->objects) {
            ++idx;
            if (!o.visible || (!o.gpu && o.parts.empty())) continue;
            DrawItem item;
            item.mesh = o.gpu;
            QColor c = object_colors_.isEmpty() ? QColor(180, 184, 190)
                                                : object_colors_[(idx - 1) % object_colors_.size()].value<QColor>();
            if (!o.manifold) c = QColor(200, 90, 80);  // non-manifold imports stand out
            item.color[0] = float(c.redF()), item.color[1] = float(c.greenF()), item.color[2] = float(c.blueF()), item.color[3] = 1;
            QString qn = QString::fromStdString(o.name);
            bool sel = selection_.contains(qn) || (tool_ == "pick" && picked_tools_.contains(qn));
            item.highlight = sel ? 1.f : 0.f;
            if (sel && tool_ == "move" && base_) item.extra_offset = snap_.point - *base_;
            if (o.gpu) {
                in.items.push_back(item);
                continue;
            }
            // sculpt parts; chunks of a running stroke replace their evaluated version
            item.edges = false;
            const LiveParts* live = live_.object == o.name ? &live_ : nullptr;
            for (std::size_t i = 0; i < o.parts.size(); ++i) {
                if (live && live->chunks.count(sdf_chunk_key(o.part_chunks[i]))) continue;
                item.mesh = o.parts[i];
                in.items.push_back(item);
            }
            if (live)
                for (const auto& [k, m] : live->chunks)
                    if (m) {
                        item.mesh = m;
                        in.items.push_back(item);
                    }
        }
    }
    if (ghost_) {
        DrawItem g;
        g.mesh = ghost_;
        g.ghost = true;
        g.edges = false;
        g.color[0] = 0.35f, g.color[1] = 0.75f, g.color[2] = 1.0f, g.color[3] = 0.35f;
        in.items.push_back(g);
    }
    renderer_->set_input(std::move(in));
}

QSGNode* ViewportItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    // Render thread, after sync(): size the offscreen image and show it as a texture.
    auto* node = static_cast<QSGSimpleTextureNode*>(old);
    if (!renderer_) return node;
    const double k = dpr();
    QSize px(int(std::round(width() * k)), int(std::round(height() * k)));
    VkImage img = VK_NULL_HANDLE;
    try {
        img = renderer_->prepare(px);
    } catch (const std::exception& e) {
        qCritical("viewport renderer: %s", e.what());
    }
    if (!img) {
        delete node;
        shown_image_ = VK_NULL_HANDLE;
        return nullptr;
    }
    if (!node) {
        node = new QSGSimpleTextureNode;
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Nearest);  // 1:1 device pixels
    }
    if (img != shown_image_) {
        node->setTexture(QNativeInterface::QSGVulkanTexture::fromNative(
            img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, window(), px));
        shown_image_ = img;
    }
    node->setRect(boundingRect());
    return node;
}

void ViewportItem::cleanup() {
    delete renderer_;  // render thread, context still valid
    renderer_ = nullptr;
    shown_image_ = VK_NULL_HANDLE;
}

void ViewportItem::releaseResources() {
    if (window() && renderer_) window()->scheduleRenderJob(new CleanupJob(renderer_), QQuickWindow::BeforeSynchronizingStage);
    renderer_ = nullptr;
}

void ViewportItem::requestFrame() {
    update();  // re-runs updatePaintNode (size changes); the texture content changes every frame anyway
    if (window()) window()->update();
}

void ViewportItem::geometryChange(const QRectF& n, const QRectF& o) {
    QQuickItem::geometryChange(n, o);
    cam_.width = n.width() * dpr();
    cam_.height = n.height() * dpr();
    emit cameraChanged();
    requestFrame();
}

void ViewportItem::setScene(std::shared_ptr<const Scene> s) {
    bool first = !scene_ && s && !s->bounds.empty();
    scene_ = std::move(s);
    {
        QStringList so;
        if (scene_)
            for (const auto& o : scene_->objects)
                if (o.grid) so << QString::fromStdString(o.name);
        if (so != sculpt_objects_) {
            sculpt_objects_ = so;
            emit sculptObjectsChanged();
        }
    }
    // a finished stroke stays visible until its command's evaluation arrives
    if (live_.done && scene_) {
        bool committed = true;
        for (const auto& o : scene_->objects)
            if (o.name == live_.object && o.root == stroke_root_) committed = false;
        if (committed) {
            live_ = LiveParts{};
            pending_grid_.reset();
            expected_root_.reset();
        }
    }
    if (first) frameAll();
    snap_engine_.reset();
    requestFrame();
    // uploads finish asynchronously on the render side; keep frames coming briefly
    QMetaObject::invokeMethod(this, [this] { requestFrame(); }, Qt::QueuedConnection);
}

void ViewportItem::setGhost(std::shared_ptr<const MeshCpu> g) {
    ghost_ = std::move(g);
    requestFrame();
}

void ViewportItem::setSelection(const QStringList& s) {
    if (s == selection_) return;
    selection_ = s;
    emit selectionChanged();
    requestFrame();
}

QString ViewportItem::snapKind() const {
    switch (snap_.type) {
        case SnapType::Vertex: case SnapType::Origin: return "point";
        case SnapType::Midpoint: return "mid";
        case SnapType::Center: case SnapType::Centroid: return "center";
        case SnapType::Intersection: return "cross";
        case SnapType::Tangent: case SnapType::Perpendicular: return "constraint";
        case SnapType::Axis: return "axis";
        case SnapType::Grid: return "grid";
        default: return "free";
    }
}

QString ViewportItem::toolHint() const {
    if (tool_ == "move")
        return base_ ? "Move: click the destination point (Esc cancels)" : "Move: click the base point (Esc cancels)";
    if (tool_ == "draw") {
        if (!draw_.have_plane) return "Click to start (on a face or the ground)  ·  Alt: no snapping  ·  Esc cancels";
        if (draw_.kind == "rect") return "Click the opposite corner (or type a length)";
        if (draw_.kind == "circle") return "Click a point on the circle (or type the radius)";
        return "Click the next point, the first point to close  ·  Enter/double-click finishes  ·  Backspace undoes  ·  type a length";
    }
    if (tool_ == "pick")
        return "Cut with: click the objects to subtract, then Enter or double-click (Esc cancels)";
    return {};
}

QPointF ViewportItem::projectPoint(const Vec3& p) const {
    Camera c = cam_;
    c.width = width() * dpr();
    c.height = height() * dpr();
    double x = 0, y = 0;
    c.project(p, x, y);
    return {x / dpr(), y / dpr()};
}

QPointF ViewportItem::basePos() const {
    if (!base_) return {};
    double x, y;
    if (!cam_.project(*base_, x, y)) return {};
    return {x / dpr(), y / dpr()};
}

QVector3D ViewportItem::camRight() const { Vec3 v = cam_.right(); return QVector3D(float(v.x), float(v.y), float(v.z)); }
QVector3D ViewportItem::camUp() const { Vec3 v = cam_.up(); return QVector3D(float(v.x), float(v.y), float(v.z)); }
QVector3D ViewportItem::camForward() const { Vec3 v = cam_.forward(); return QVector3D(float(v.x), float(v.y), float(v.z)); }

QVariantList ViewportItem::radialPath() const {
    QVariantList l;
    for (int i : radial_.path()) l << i;
    return l;
}

void ViewportItem::updateSnap(QPointF pos) {
    if (!scene_) {
        snap_visible_ = false;
        emit snapChanged();
        return;
    }
    SnapScene sc;
    for (const auto& o : scene_->objects) {
        if (!o.visible) continue;
        if (o.snap) sc.objects.push_back(o.snap);
        for (const auto& ps : o.part_snaps) sc.objects.push_back(ps);
    }
    SnapQuery q;
    q.cam = cam_;
    q.cam.width = width() * dpr();
    q.cam.height = height() * dpr();
    q.mx = pos.x() * dpr();
    q.my = pos.y() * dpr();
    q.disabled = QGuiApplication::queryKeyboardModifiers() & Qt::AltModifier;
    if (tool_ == "move" && base_) {
        q.base = base_;
        if (!selection_.isEmpty()) q.ignore_object = selection_.front().toStdString();
    }
    snap_ = snap_engine_.update(sc, q);
    snap_visible_ = snap_.type != SnapType::None;
    emit snapChanged();
    if (tool_ == "move" && base_) requestFrame();
}

// ------------------------------------------------------------------ input
void ViewportItem::mousePressEvent(QMouseEvent* e) {
    forceActiveFocus();
    press_pos_ = last_pos_ = e->position();
    drag_button_ = e->button();
    dragging_ = false;
    if (e->button() == Qt::LeftButton && tool_ == "sculpt" && sculptPress(e)) {
        e->accept();
        return;
    }
    if (e->button() == Qt::RightButton) {
        hover_pos_ = e->position();
        updateSnap(hover_pos_);
        radial_by_mouse_ = true;
        radialBegin(selection_.isEmpty() ? "empty" : "selection");
    }
    e->accept();
}

void ViewportItem::mouseMoveEvent(QMouseEvent* e) {
    QPointF p = e->position();
    if (sculpting_) {
        sculptMove(p);
        return;
    }
    if (drag_button_ == Qt::RightButton && radial_.active()) {
        hover_pos_ = p;
        if (radial_.move(p.x(), p.y())) emit radialChanged();
        return;
    }
    if (!dragging_ && QLineF(p, press_pos_).length() > kDragThresholdPx) dragging_ = true;
    if (dragging_) {
        QPointF delta = p - last_pos_;
        bool pan = drag_button_ == Qt::MiddleButton ||
                   (drag_button_ == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier));
        if (pan) cam_.pan_pixels(delta.x() * dpr(), delta.y() * dpr());
        else cam_.orbit(-delta.x() * kOrbitRadPerPx, delta.y() * kOrbitRadPerPx);
        animating_ = false;
        emit cameraChanged();
        requestFrame();
    }
    last_pos_ = p;
}

void ViewportItem::mouseReleaseEvent(QMouseEvent* e) {
    if (sculpting_ && e->button() == Qt::LeftButton) {
        sculptRelease();
        drag_button_ = Qt::NoButton;
        return;
    }
    if (e->button() == Qt::RightButton && radial_by_mouse_) {
        hover_pos_ = e->position();
        radial_by_mouse_ = false;
        radialEnd();
        drag_button_ = Qt::NoButton;
        return;
    }
    if (!dragging_ && e->button() == Qt::LeftButton) {
        updateSnap(e->position());
        if (tool_ == "move") {
            if (!base_) {
                base_ = snap_.point;
                emit toolChanged();
            } else if (!selection_.isEmpty()) {
                Vec3 delta = snap_.point - *base_;
                emit moveRequested(selection_.front(), delta.x, delta.y, delta.z);
                cancelTool();
            }
        } else if (tool_ == "draw") {
            hover_pos_ = e->position();
            drawMove(hover_pos_);
            drawClick(false);
        } else if (tool_ == "pick") {
            QString o = QString::fromStdString(snap_.object);
            if (o.isEmpty()) o = pickSketchAt(e->position().x(), e->position().y());
            if (!o.isEmpty() && o != pick_target_) {
                if (picked_tools_.contains(o)) picked_tools_.removeAll(o);
                else picked_tools_ << o;
                emit toolChanged();
                requestFrame();
                if (pick_single_) finishPick();
            }
        } else {
            bool add = e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
            QString o = QString::fromStdString(snap_.object);
            // sketches have no faces: fall back to their lines in screen space
            if (o.isEmpty()) o = pickSketchAt(e->position().x(), e->position().y());
            emit picked(o, add);
        }
    }
    dragging_ = false;
    drag_button_ = Qt::NoButton;
}

void ViewportItem::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    if (tool_ == "draw") {
        drawClick(true);
        return;
    }
    if (tool_ == "pick") {
        finishPick();
        return;
    }
    QString o = QString::fromStdString(snap_.object);
    if (o.isEmpty()) o = pickSketchAt(e->position().x(), e->position().y());
    if (o.isEmpty()) frameAll();
    else emit objectDoubleClicked(o);
}

void ViewportItem::hoverMoveEvent(QHoverEvent* e) {
    hover_pos_ = e->position();
    if (radial_.active()) {
        if (radial_.move(hover_pos_.x(), hover_pos_.y())) emit radialChanged();
        return;
    }
    if (tool_ == "draw") drawMove(hover_pos_);
    else if (tool_ == "sculpt") sculptHover(hover_pos_);
    else updateSnap(hover_pos_);
}

void ViewportItem::wheelEvent(QWheelEvent* e) {
    double notches = e->angleDelta().y() / 120.0;
    if (notches == 0) return;
    QPointF p = e->position();
    cam_.width = width() * dpr();
    cam_.height = height() * dpr();
    cam_.zoom_at(std::pow(kWheelStep, notches), p.x() * dpr(), p.y() * dpr());
    emit cameraChanged();
    requestFrame();
    if (tool_ == "draw") drawMove(p);
    else updateSnap(p);
}

// ------------------------------------------------------------------ radial
void ViewportItem::radialBegin(QString context) {
    if (!radial_rings_) return;
    if (context_resolver_) context = context_resolver_(context);
    radial_context_ = context;
    radial_point_ = snapPoint();
    radial_point_exact_ = snap_visible_ ? snap_.point : Vec3{};
    radial_.begin(hover_pos_.x(), hover_pos_.y(), [this](const std::vector<int>& path) { return radial_rings_(path); });
    emit radialChanged();
}

void ViewportItem::radialEnd() {
    if (!radial_.active()) return;
    auto sel = radial_.end(hover_pos_.x(), hover_pos_.y());
    emit radialChanged();
    if (sel) {
        QVariantList path;
        for (int i : *sel) path << i;
        emit radialSelected(path, radial_context_, radial_point_);
    }
}

void ViewportItem::radialCancel() {
    radial_.cancel();
    emit radialChanged();
}

// ------------------------------------------------------------------ tools / camera
void ViewportItem::startMoveTool() {
    if (selection_.isEmpty()) return;
    tool_ = "move";
    base_.reset();
    emit toolChanged();
}

void ViewportItem::cancelTool() {
    bool was_draw = tool_ == "draw";
    tool_ = "select";
    base_.reset();
    picked_tools_.clear();
    pick_target_.clear();
    if (was_draw) {
        draw_ = Draw{};
        emit drawChanged();
    }
    emit toolChanged();
    requestFrame();
}

void ViewportItem::animateTo(double yaw, double pitch) {
    double dy = std::remainder(yaw - cam_.yaw, 2 * std::numbers::pi);  // shortest way round
    anim_from_[0] = cam_.yaw, anim_from_[1] = cam_.pitch;
    anim_to_[0] = cam_.yaw + dy, anim_to_[1] = pitch;
    animating_ = true;
    anim_clock_.start();
    requestFrame();
}

void ViewportItem::viewDirection(double x, double y, double z) {
    Camera c = cam_;
    c.look_from({x, y, z});
    animateTo(c.yaw, c.pitch);
}

void ViewportItem::view(const QString& n) {
    if (n == "top") viewDirection(0, 0, 1);
    else if (n == "bottom") viewDirection(0, 0, -1);
    else if (n == "front") viewDirection(0, -1, 0);
    else if (n == "back") viewDirection(0, 1, 0);
    else if (n == "left") viewDirection(-1, 0, 0);
    else if (n == "right") viewDirection(1, 0, 0);
    else if (n == "iso") viewDirection(1, -1, 1);
    else if (n == "frame") frameAll();
    else if (n == "ortho" || n == "perspective") {
        cam_.ortho = n == "ortho";
        emit cameraChanged();
        requestFrame();
    }
}

QVector3D ViewportItem::cubeDirection(double u, double v) const {
    // The widget shows an orthographic projection of [-1,1]^3 with scale kCubeScale.
    Vec3 r = cam_.right(), up = cam_.up(), f = cam_.forward();
    Vec3 o = r * (u / kCubeScale) + up * (v / kCubeScale) - f * 10.0;
    double t0 = -INFINITY, t1 = INFINITY;
    for (int a = 0; a < 3; ++a) {
        if (std::abs(f[a]) < 1e-12) {
            if (std::abs(o[a]) > 1) return {};
            continue;
        }
        double ta = (-1 - o[a]) / f[a], tb = (1 - o[a]) / f[a];
        t0 = std::max(t0, std::min(ta, tb));
        t1 = std::min(t1, std::max(ta, tb));
    }
    if (t0 > t1) return {};
    Vec3 hit = o + f * t0;
    auto dir3 = classify_cube_hit(hit);
    return QVector3D(float(dir3[0]), float(dir3[1]), float(dir3[2]));
}

void ViewportItem::frameAll() {
    Box3 b;
    if (scene_) b = scene_->bounds;
    if (b.empty()) b.add(Vec3{-25, -25, 0}), b.add(Vec3{25, 25, 10});
    cam_.frame(b);
    emit cameraChanged();
    requestFrame();
}

void ViewportItem::frameSwapped() {
    const double dt = frame_clock_.isValid() ? frame_clock_.nsecsElapsed() / 1e6 : -1;
    frame_clock_.restart();
    // Interactive statistics only count consecutive frames (idle gaps are not
    // frame times); during a benchmark every frame is requested back-to-back,
    // so every interval counts, however long.
    if (dt > 0 && bench_left_ > 0) bench_ms_.push_back(dt), bench_gpu_.push_back(stats_->gpu_ms);
    if (dt > 0 && dt < 250) frame_ms_[frame_idx_++ % kFrameWindow] = dt;
    if (animating_) {
        double t = std::min(1.0, anim_clock_.elapsed() / kAnimMs);
        double s = t * t * (3 - 2 * t);  // smoothstep
        cam_.yaw = anim_from_[0] + (anim_to_[0] - anim_from_[0]) * s;
        cam_.pitch = anim_from_[1] + (anim_to_[1] - anim_from_[1]) * s;
        if (t >= 1) animating_ = false;
        emit cameraChanged();
        requestFrame();
    }
    if (bench_left_ > 0) {
        --bench_left_;
        double done = double(bench_total_ - bench_left_) / bench_total_;
        cam_.yaw = bench_yaw0_ + 2 * std::numbers::pi * done;
        requestFrame();
        if (bench_left_ == 0) {
            QJsonObject j;
            const auto& v = bench_ms_;
            j["frames"] = int(v.size());
            j["cpu_present_interval_ms"] = QJsonObject{{"p50", percentile(v, 0.5)}, {"p95", percentile(v, 0.95)},
                                                       {"p99", percentile(v, 0.99)},
                                                       {"max", v.empty() ? 0 : *std::max_element(v.begin(), v.end())}};
            j["gpu_ms"] = QJsonObject{{"p50", percentile(bench_gpu_, 0.5)}, {"p99", percentile(bench_gpu_, 0.99)}};
            j["triangles"] = qint64(stats_->triangles.load());
            j["device"] = QString::fromStdString(stats_->device_name);
            j["msaa"] = stats_->msaa;
            j["viewport_px"] = QJsonArray{width() * dpr(), height() * dpr()};
            j["target_p99_ms"] = 16.7;
            QString json = QString::fromUtf8(QJsonDocument(j).toJson());
            if (!bench_out_.isEmpty()) {
                QFile f(bench_out_);
                if (f.open(QIODevice::WriteOnly)) f.write(json.toUtf8());
            }
            emit benchmarkFinished(json);
        }
    }
    if (++stats_tick_ % 30 == 0) {
        std::vector<double> v;
        for (double x : frame_ms_)
            if (x > 0) v.push_back(x);
        p50_ = percentile(v, 0.5);
        p99_ = percentile(v, 0.99);
        emit statsChanged();
    }
}

void ViewportItem::runBenchmark(int frames, const QString& outPath) {
    frameAll();
    bench_total_ = bench_left_ = std::max(frames, 10);
    bench_out_ = outPath;
    bench_ms_.clear();
    bench_gpu_.clear();
    bench_yaw0_ = cam_.yaw;
    frame_clock_.invalidate();
    requestFrame();
}

}  // namespace anvil
