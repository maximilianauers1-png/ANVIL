// Sculpt tool: brush cursor, live strokes (SculptEngine), commit.
#include <QGuiApplication>
#include <QMouseEvent>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "sculpt_engine.hpp"
#include "viewport_item.hpp"

namespace anvil {

void ViewportItem::setSculptMode(bool on) {
    if (on == (tool_ == "sculpt")) return;
    if (sculpting_) sculptRelease();
    cancelTool();
    if (on) tool_ = "sculpt";
    brush_pos_ = {-1e6, -1e6};
    brush_target_.clear();
    emit toolChanged();
    emit brushChanged();
}

std::shared_ptr<const sdf::Grid> ViewportItem::sculpt_grid_of(const SceneObject& o) const {
    // a finished stroke whose command is not evaluated yet is the newer state
    if (pending_grid_ && live_.object == o.name) return pending_grid_;
    return o.grid;
}

const SceneObject* ViewportItem::sculpt_object_at(QPointF pos, double* t_hit, Vec3* p_hit) const {
    if (!scene_) return nullptr;
    Camera c = cam_;
    c.width = width() * dpr(), c.height = height() * dpr();
    const Ray r = c.ray(pos.x() * dpr(), pos.y() * dpr());
    const SceneObject* best = nullptr;
    double bt = INFINITY;
    for (const auto& o : scene_->objects) {
        if (!o.visible || !o.grid) continue;
        auto g = sculpt_grid_of(o);
        if (auto t = sdf::raycast(*g, r.origin, normalized(r.dir), 1e7); t && *t < bt) bt = *t, best = &o;
    }
    if (best) {
        if (t_hit) *t_hit = bt;
        if (p_hit) *p_hit = r.origin + normalized(r.dir) * bt;
    }
    return best;
}

bool ViewportItem::sculptReady(const QString& name) const {
    if (!scene_) return false;
    for (const auto& o : scene_->objects)
        if (o.name == name.toStdString()) return bool(o.grid);
    return false;
}

void ViewportItem::sculptHover(QPointF pos) {
    double t = 0;
    Vec3 p;
    const SceneObject* o = sculpt_object_at(pos, &t, &p);
    QString target = o ? QString::fromStdString(o->name) : QString();
    if (o) {
        // screen radius: project a point R away along the camera's right axis
        QPointF a = projectPoint(p), b = projectPoint(p + cam_.right() * sculpt_radius_);
        brush_pos_ = a;
        brush_px_ = QLineF(a, b).length();
    } else {
        brush_pos_ = {-1e6, -1e6};
    }
    brush_target_ = target;
    emit brushChanged();
}

bool ViewportItem::sculptPress(QMouseEvent* e) {
    double t = 0;
    Vec3 p;
    const SceneObject* o = sculpt_object_at(e->position(), &t, &p);
    if (!o || !sculpt_engine_) return false;  // empty space: orbit as usual
    sdf::Brush brush = sdf::Brush::Draw;
    sdf::parse_brush(sculpt_brush_.toStdString(), brush);
    const auto mods = e->modifiers();
    if (mods & Qt::ShiftModifier) brush = sdf::Brush::Smooth;  // sculpting convention
    else if ((mods & Qt::ControlModifier) && brush == sdf::Brush::Draw) brush = sdf::Brush::Carve;
    else if ((mods & Qt::ControlModifier) && brush == sdf::Brush::Carve) brush = sdf::Brush::Draw;
    live_.object = o->name;
    live_.chunks.clear();
    live_.done = false;
    stroke_root_ = o->root;
    sculpting_ = true;
    stroke_t_ = t;
    grab_plane_p_ = p;
    grab_plane_n_ = cam_.forward();
    sculpt_engine_->begin(o->name, sculpt_grid_of(*o), brush, sculpt_radius_, sculpt_strength_);
    sculpt_engine_->add(p);
    return true;
}

bool ViewportItem::sculptMove(QPointF pos) {
    Camera c = cam_;
    c.width = width() * dpr(), c.height = height() * dpr();
    const Ray r = c.ray(pos.x() * dpr(), pos.y() * dpr());
    const Vec3 d = normalized(r.dir);
    Vec3 p;
    if (sculpt_brush_ == "grab") {  // the grabbed surface follows the pointer in the screen plane
        double den = dot(d, grab_plane_n_);
        if (std::abs(den) < 1e-9) return true;
        p = r.origin + d * (dot(grab_plane_p_ - r.origin, grab_plane_n_) / den);
    } else {
        double t = 0;
        Vec3 hit;
        if (sculpt_object_at(pos, &t, &hit)) stroke_t_ = t, p = hit;
        else p = r.origin + d * stroke_t_;  // off the silhouette: keep the depth
    }
    sculpt_engine_->add(p);
    sculptHover(pos);
    return true;
}

void ViewportItem::sculptRelease() {
    if (!sculpting_) return;
    sculpting_ = false;
    sculpt_engine_->end();
}

}  // namespace anvil
