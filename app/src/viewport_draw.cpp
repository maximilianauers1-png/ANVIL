// Drawing tools (polyline / rectangle / circle) and 'Cut with...' picking.
//
// Streamlined by design: the sketch plane is taken from what the first click
// lands on (ground, or the face under the cursor), points snap to existing
// geometry, to a plane grid and to horizontal/vertical from the previous point,
// and finishing produces exactly one Python command (sketch_polyline, ...).
#include <QGuiApplication>

#include <cmath>

#include "anvil/tolerance.hpp"

#include "viewport_item.hpp"
#include "view_util.hpp"

namespace anvil {

using namespace view_util;

namespace {
constexpr double kInferPx = 8.0;     // horizontal/vertical inference radius
constexpr double kClosePx = 10.0;    // clicking this close to the first point closes the polyline
constexpr double kGridSnapPx = 8.0;  // grid snap radius
constexpr double kLinePickPx = 6.0;  // sketch line picking radius

}  // namespace

void ViewportItem::setPlaneFrame(SketchPlane p, double offset) {
    Sketch s;
    s.plane = p;
    offset = clean(offset);
    s.offset = offset;
    s.frame(draw_.o, draw_.u, draw_.v, draw_.n);
    draw_.plane = p;
    draw_.offset = offset;
    draw_.have_plane = true;
}

std::optional<std::array<double, 2>> ViewportItem::rayToPlane(QPointF pos) const {
    Camera c = cam_;
    c.width = width() * dpr(), c.height = height() * dpr();
    Ray r = c.ray(pos.x() * dpr(), pos.y() * dpr());
    double den = dot(r.dir, draw_.n);
    if (std::abs(den) < 1e-9) return std::nullopt;
    double t = dot(draw_.o - r.origin, draw_.n) / den;
    Vec3 p = r.origin + r.dir * t;
    return std::array<double, 2>{dot(p - draw_.o, draw_.u), dot(p - draw_.o, draw_.v)};
}

QPointF ViewportItem::sketchToItem(double x, double y) const {
    return projectPoint(draw_.o + draw_.u * x + draw_.v * y);
}

QPointF ViewportItem::projectWorld(const QVector3D& p) const { return projectPoint({p.x(), p.y(), p.z()}); }

QVariant ViewportItem::planePoint(double x, double y, const QString& plane, double offset) const {
    auto pl = parse_plane(plane.toStdString());
    if (!pl) return {};
    Sketch s;
    s.plane = *pl, s.offset = offset;
    Vec3 o, u, v, n;
    s.frame(o, u, v, n);
    Camera c = cam_;
    c.width = width() * dpr(), c.height = height() * dpr();
    Ray r = c.ray(x * dpr(), y * dpr());
    double den = dot(r.dir, n);
    if (std::abs(den) < 1e-9) return {};
    Vec3 p = r.origin + r.dir * (dot(o - r.origin, n) / den);
    return QPointF(dot(p - o, u), dot(p - o, v));
}

QVariantList ViewportItem::drawPoints() const {
    QVariantList l;
    for (const auto& p : draw_.pts) l << sketchToItem(p[0], p[1]);
    return l;
}

double ViewportItem::drawSegment() const {
    if (draw_.pts.empty() || !draw_.cursor_valid) return 0;
    const auto& a = draw_.pts.back();
    return std::hypot(draw_.cursor[0] - a[0], draw_.cursor[1] - a[1]);
}

void ViewportItem::startDraw(const QString& kind) {
    cancelTool();
    tool_ = "draw";
    draw_ = Draw{};
    draw_.kind = kind;
    emit toolChanged();
    emit drawChanged();
}

void ViewportItem::drawMove(QPointF pos) {
    if (!draw_.have_plane) {  // before the first click: plain snapping shows where we would start
        updateSnap(pos);
        return;
    }
    updateSnap(pos);
    auto hit = rayToPlane(pos);
    if (!hit) return;
    std::array<double, 2> c = *hit;
    draw_.guide.clear();
    // 1. snapped scene point lying on the sketch plane
    bool snapped = false;
    if (snap_visible_ && snap_.type != SnapType::Plane && snap_.type != SnapType::Normal &&
        std::abs(dot(snap_.point - draw_.o, draw_.n)) < 1e-6) {
        c = {dot(snap_.point - draw_.o, draw_.u), dot(snap_.point - draw_.o, draw_.v)};
        snapped = true;
    }
    const double wpp = cam_.world_per_pixel(cam_.distance) * dpr();
    // 2. own points (close the polyline)
    if (!draw_.pts.empty() && draw_.kind == "polyline" && draw_.pts.size() >= 3) {
        QPointF f = sketchToItem(draw_.pts[0][0], draw_.pts[0][1]);
        if (QLineF(f, pos).length() < kClosePx) c = draw_.pts[0], snapped = true, draw_.guide = "close";
    }
    // 3. horizontal / vertical from the previous point
    if (!snapped && !draw_.pts.empty() && draw_.kind != "circle") {
        const auto& a = draw_.pts.back();
        if (std::abs(c[1] - a[1]) / wpp < kInferPx) c[1] = a[1], draw_.guide = "horizontal";
        else if (std::abs(c[0] - a[0]) / wpp < kInferPx) c[0] = a[0], draw_.guide = "vertical";
    }
    // 4. plane grid (adaptive: about 12 px)
    if (!snapped && !(QGuiApplication::queryKeyboardModifiers() & Qt::AltModifier)) {
        double g = nice_step(12 * wpp);
        for (int k = 0; k < 2; ++k) {
            if (draw_.guide == "horizontal" && k == 1) continue;
            if (draw_.guide == "vertical" && k == 0) continue;
            double r = std::round(c[k] / g) * g;
            if (std::abs(r - c[k]) / wpp < kGridSnapPx) c[k] = r;
            else c[k] = quantize_px(c[k], wpp);
        }
    } else if (!snapped) {
        c[0] = quantize_px(c[0], wpp), c[1] = quantize_px(c[1], wpp);
    }
    c[0] = clean(c[0]), c[1] = clean(c[1]);
    draw_.cursor = c;
    draw_.cursor_valid = true;
    emit drawChanged();
}

void ViewportItem::drawClick(bool double_click) {
    if (!draw_.have_plane) {
        // plane from what is under the cursor: an axis-aligned face, else the ground
        Vec3 n = snap_.normal, p = snap_.point;
        if (!snap_.object.empty() && std::abs(n.z) > 0.999) setPlaneFrame(SketchPlane::XY, p.z);
        else if (!snap_.object.empty() && std::abs(n.y) > 0.999) setPlaneFrame(SketchPlane::XZ, -p.y);
        else if (!snap_.object.empty() && std::abs(n.x) > 0.999) setPlaneFrame(SketchPlane::YZ, p.x);
        else setPlaneFrame(SketchPlane::XY, snap_visible_ ? p.z : 0.0);
        draw_.on = QString::fromStdString(snap_.object);
        drawMove(hover_pos_);
    }
    if (!draw_.cursor_valid) return;
    if (draw_.guide == "close") {
        draw_.pts.push_back(draw_.cursor);
        finishDraw();
        return;
    }
    if (double_click) {  // the double-click's first click already added this point
        finishDraw();
        return;
    }
    if (!draw_.pts.empty() && std::hypot(draw_.cursor[0] - draw_.pts.back()[0], draw_.cursor[1] - draw_.pts.back()[1]) < 1e-9)
        return;  // ignore duplicate points
    draw_.pts.push_back(draw_.cursor);
    emit drawChanged();
    if ((draw_.kind == "rect" || draw_.kind == "circle") && draw_.pts.size() == 2) finishDraw();
}

void ViewportItem::drawTypedLength(double mm) {
    if (tool_ != "draw" || draw_.pts.empty() || !draw_.cursor_valid || !(mm > 0)) return;
    const auto& a = draw_.pts.back();
    double dx = draw_.cursor[0] - a[0], dy = draw_.cursor[1] - a[1], l = std::hypot(dx, dy);
    if (l < 1e-12) dx = 1, dy = 0, l = 1;
    draw_.cursor = {a[0] + dx / l * mm, a[1] + dy / l * mm};
    draw_.guide.clear();
    drawClick(false);
}

void ViewportItem::drawUndoPoint() {
    if (tool_ == "draw" && !draw_.pts.empty()) {
        draw_.pts.pop_back();
        emit drawChanged();
    }
}

void ViewportItem::finishDraw() {
    if (tool_ != "draw") return;
    bool closed = false;
    auto pts = draw_.pts;
    if (draw_.kind == "polyline" && pts.size() >= 4 &&
        std::hypot(pts.front()[0] - pts.back()[0], pts.front()[1] - pts.back()[1]) < 1e-9) {
        closed = true;
        pts.pop_back();
    }
    bool enough = draw_.kind == "polyline" ? pts.size() >= 2 : pts.size() == 2;
    QVariantList l;
    for (const auto& p : pts) l << QVariant::fromValue(QPointF(p[0], p[1]));
    QString kind = draw_.kind, plane = plane_name(draw_.plane), on = draw_.on;
    double offset = draw_.offset;
    cancelTool();
    draw_ = Draw{};
    emit drawChanged();
    if (enough) emit sketchDrawn(kind, plane, offset, l, closed, on);
}

void ViewportItem::startPickTools(const QString& target, bool single) {
    cancelTool();
    tool_ = "pick";
    pick_target_ = target;
    pick_single_ = single;
    picked_tools_.clear();
    emit toolChanged();
}

void ViewportItem::finishPick() {
    if (tool_ != "pick") return;
    QString target = pick_target_;
    QStringList tools = picked_tools_;
    cancelTool();
    if (!tools.isEmpty()) emit toolsPicked(target, tools);
}

QString ViewportItem::pickSketchAt(double x, double y) const {
    // sketches have no surfaces: pick them by their lines in screen space
    if (!scene_) return {};
    QString best;
    double bd = kLinePickPx;
    for (const auto& o : scene_->objects) {
        if (!o.visible || !o.gpu || !o.gpu->tris.empty()) continue;
        const auto& g = *o.gpu;
        for (std::size_t i = 0; i + 1 < g.edges.size(); i += 2) {
            auto P = [&](std::uint32_t k) {
                return projectPoint(g.origin + Vec3{g.pos[3 * k], g.pos[3 * k + 1], g.pos[3 * k + 2]});
            };
            QPointF a = P(g.edges[i]), b = P(g.edges[i + 1]);
            QPointF ab = b - a, ap = QPointF(x, y) - a;
            double l2 = QPointF::dotProduct(ab, ab);
            double t = l2 > 0 ? std::clamp(QPointF::dotProduct(ap, ab) / l2, 0.0, 1.0) : 0.0;
            double dist = QLineF(a + ab * t, QPointF(x, y)).length();
            if (dist < bd) bd = dist, best = QString::fromStdString(o.name);
        }
    }
    return best;
}

}  // namespace anvil
