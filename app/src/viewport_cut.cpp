// Cut tools in the viewport: draw a path (knife: clicks, line: two clicks,
// lasso: drag a loop), then click the part to keep (Shift: keep both).
// Drill: click a point; slice: click a height, then the side to keep.
// The preview region is the core's own region (cut::region) projected to the
// screen, and the kept side is decided on that projection, so what is
// highlighted is exactly what is kept.
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPolygonF>

#include <cmath>
#include <cstdio>

#include "anvil/cut.hpp"
#include "viewport_item.hpp"

namespace anvil {

namespace {
constexpr double kLassoStepPx = 3.0;   // spacing of freehand lasso points
constexpr double kSamplePx = 4.0;      // spacing of the target-finding samples along a path
}  // namespace

QString ViewportItem::cutModeOf() const {
    if (cut_.kind == "lasso" || cut_.kind == "drill") return "lasso";
    if (cut_.kind == "slice") return "plane";
    return "knife";
}

QVariantList ViewportItem::cutPoints() const {
    QVariantList l;
    for (const auto& p : cut_.pts) l << p;
    return l;
}

std::optional<std::pair<std::string, Vec3>> ViewportItem::rayPick(QPointF pos) const {
    if (!scene_) return std::nullopt;
    Camera c = cam_;
    c.width = width() * dpr(), c.height = height() * dpr();
    const Ray r = c.ray(pos.x() * dpr(), pos.y() * dpr());
    const Vec3 d = normalized(r.dir);
    double best = INFINITY;
    std::optional<std::pair<std::string, Vec3>> out;
    for (const auto& o : scene_->objects) {
        if (!o.visible) continue;
        auto test = [&](const std::shared_ptr<const SnapObject>& so) {
            if (!so || !so->bvh) return;
            if (auto h = so->bvh->raycast(r.origin, d); h && h->t < best) best = h->t, out = std::make_pair(o.name, h->point);
        };
        test(o.snap);
        for (const auto& ps : o.part_snaps) test(ps);
    }
    return out;
}

void ViewportItem::startCut(const QString& kind) {
    cancelTool();
    tool_ = "cut";
    cut_ = Cut{};
    cut_.kind = kind;
    emit toolChanged();
    emit cutChanged();
}

void ViewportItem::cutUndoPoint() {
    if (tool_ != "cut" || cut_.choosing || cut_.pts.empty()) return;
    cut_.pts.pop_back();
    emit cutChanged();
}

void ViewportItem::finishCutPath() {
    if (tool_ != "cut" || cut_.choosing) return;
    if (cut_.kind == "knife" && cut_.pts.size() >= 2) cutEnterChoosing();
}

void ViewportItem::cutEnterChoosing() {
    if (!scene_) return;
    // targets: the selection, else every object the path runs over (lasso: also inside it)
    QStringList targets;
    for (const auto& s : selection_)
        for (const auto& o : scene_->objects)
            if (o.visible && QString::fromStdString(o.name) == s && (o.gpu || !o.parts.empty())) targets << s;
    if (targets.isEmpty()) {
        auto add = [&](QPointF p) {
            if (auto h = rayPick(p)) {
                const QString n = QString::fromStdString(h->first);
                if (!targets.contains(n)) targets << n;
            }
        };
        std::vector<QPointF> path = cut_.pts;
        if (cut_.kind == "lasso" && !path.empty()) path.push_back(path.front());
        if (path.size() == 1) add(path.front());  // slice: the clicked object
        for (std::size_t i = 0; i + 1 < path.size(); ++i) {
            const int n = std::max(1, int(QLineF(path[i], path[i + 1]).length() / kSamplePx));
            for (int k = 0; k <= n; ++k) add(path[i] + (path[i + 1] - path[i]) * (double(k) / n));
        }
        if (cut_.kind == "lasso") {
            QPolygonF poly;
            for (const auto& p : cut_.pts) poly << p;
            const QRectF br = poly.boundingRect();
            for (double y = br.top(); y <= br.bottom(); y += 2 * kSamplePx)
                for (double x = br.left(); x <= br.right(); x += 2 * kSamplePx)
                    if (poly.containsPoint({x, y}, Qt::OddEvenFill)) add({x, y});
        }
    }
    if (targets.isEmpty()) {  // drawn beside every object: start over
        cut_.pts.clear();
        emit cutChanged();
        return;
    }
    Box3 bb;
    for (const auto& o : scene_->objects)
        if (targets.contains(QString::fromStdString(o.name))) bb.add(o.bounds);
    cut_.targets = targets;
    cut_.reach = bb.diagonal() + 1.0;
    cut::Spec spec;
    spec.mode = cutModeOf().toStdString();
    Camera c = cam_;
    c.width = width() * dpr(), c.height = height() * dpr();
    if (cut_.kind == "slice") {
        // horizontal plane through the clicked point (path[0] is the world point)
        spec.dir = {0, 0, 1};
        spec.path = cut_.world;
    } else {
        cut_.plane_n = cam_.forward();
        cut_.plane_o = bb.center();
        cut_.world.clear();
        for (const auto& p : cut_.pts) {
            const Ray r = c.ray(p.x() * dpr(), p.y() * dpr());
            const Vec3 d = normalized(r.dir);
            const double den = dot(d, cut_.plane_n);
            if (std::abs(den) < 1e-12) continue;
            cut_.world.push_back(r.origin + d * (dot(cut_.plane_o - r.origin, cut_.plane_n) / den));
        }
        spec.dir = cut_.plane_n;
        spec.path = cut_.world;
        cut_.eye.reset();
        if (!cam_.ortho && cut_.kind != "drill") {
            cut_.eye = cam_.eye();
            spec.has_eye = true;
            spec.eye = *cut_.eye;
        }
    }
    // preview: the region polygons in screen coordinates
    cut_.region_screen.clear();
    try {
        if (spec.mode == "plane") {
            const Vec3 p = spec.path.front();
            Vec3 side = cross(Vec3{0, 0, 1}, cam_.forward());
            if (norm(side) < 1e-6) side = cam_.right();
            side = normalized(side) * (4 * cut_.reach);
            const QPointF a = projectPoint(p - side), b = projectPoint(p + side);
            QPointF up = projectPoint(p + Vec3{0, 0, cut_.reach}) - projectPoint(p);
            const double l = std::hypot(up.x(), up.y());
            if (l < 1e-9) up = {0, -1};
            else up /= l;
            const double far = 4 * std::max(width(), height());
            QVariantList poly{a, b, b + up * far, a + up * far};
            cut_.region_screen << QVariant(poly);
        } else {
            const cut::Frame fr = cut::frame(spec);
            for (const auto& reg : cut::region(spec, cut_.reach)) {
                QVariantList poly;
                for (const auto& q : reg) poly << projectPoint(fr.o + fr.u * q[0] + fr.v * q[1]);
                cut_.region_screen << QVariant(poly);
            }
        }
    } catch (const std::exception&) {
        cut_.pts.clear();  // degenerate path
        emit cutChanged();
        return;
    }
    cut_.choosing = true;
    cut_.hover = -1;
    emit toolChanged();
    emit cutChanged();
}

bool ViewportItem::cutPress(QMouseEvent* e) {
    if (cut_.kind == "lasso" && !cut_.choosing) {
        cut_.pts = {e->position()};
        cut_.freehand = true;
        emit cutChanged();
        return true;
    }
    return false;
}

bool ViewportItem::cutMove(QPointF pos, bool) {
    cut_.cursor = pos;
    if (cut_.freehand) {
        if (QLineF(cut_.pts.back(), pos).length() >= kLassoStepPx) cut_.pts.push_back(pos);
    } else if (cut_.choosing) {
        int h = 0;
        for (const auto& pv : cut_.region_screen) {
            QPolygonF poly;
            for (const auto& q : pv.toList()) poly << q.toPointF();
            if (poly.containsPoint(pos, Qt::WindingFill)) h = 1;
        }
        cut_.hover = h;
    } else if (cut_.kind == "drill" || cut_.kind == "slice") {
        updateSnap(pos);
    }
    emit cutChanged();
    return true;
}

bool ViewportItem::cutRelease(QMouseEvent* e) {
    const QPointF pos = e->position();
    if (cut_.skip_release) {  // the release that belongs to a double-click
        cut_.skip_release = false;
        return true;
    }
    if (cut_.freehand) {
        cut_.freehand = false;
        if (cut_.pts.size() >= 3) cutEnterChoosing();
        else cut_.pts.clear(), emit cutChanged();
        return true;
    }
    if (dragging_) return false;  // an orbit
    if (cut_.choosing) {
        cutMove(pos, false);
        const QString mode = cutModeOf();
        const bool region = cut_.hover == 1;
        QString keep = mode == "knife" ? (region ? "left" : "right")
                       : mode == "lasso" ? (region ? "inside" : "outside")
                                         : (region ? "above" : "below");
        if (e->modifiers() & Qt::ShiftModifier) keep = "both";
        const Vec3 d = cut_.kind == "slice" ? Vec3{0, 0, 1} : cut_.plane_n;
        // world points as double triples (QVector3D would round to float)
        QVariantList exact;
        for (const auto& w : cut_.world) exact << QVariant(QVariantList{w.x, w.y, w.z});
        QVariantList eye;
        if (cut_.eye) eye = {cut_.eye->x, cut_.eye->y, cut_.eye->z};
        emit cutRequested(cut_.targets, exact, QVector3D(float(d.x), float(d.y), float(d.z)), mode, keep, eye);
        const QString kind = cut_.kind;
        cut_ = Cut{};
        cut_.kind = kind;  // stay in the tool for the next cut (Esc leaves)
        emit toolChanged();
        emit cutChanged();
        return true;
    }
    if (cut_.kind == "drill" || cut_.kind == "slice") {
        auto h = rayPick(pos);
        if (!h) return true;
        if (cut_.kind == "drill") {
            const Vec3 f = cam_.forward();
            emit drillRequested(QString::fromStdString(h->first), QVector3D(float(h->second.x), float(h->second.y), float(h->second.z)),
                                QVector3D(float(f.x), float(f.y), float(f.z)), drill_d_);
            return true;
        }
        cut_.pts = {pos};
        cut_.world = {h->second};
        cutEnterChoosing();
        return true;
    }
    if (!cut_.pts.empty() && QLineF(cut_.pts.back(), pos).length() < 2) return true;  // double-click's second press
    cut_.pts.push_back(pos);
    emit cutChanged();
    if (cut_.kind == "line" && cut_.pts.size() == 2) cutEnterChoosing();
    return true;
}

}  // namespace anvil
