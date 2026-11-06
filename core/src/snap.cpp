#include "anvil/snap.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "anvil/tolerance.hpp"

namespace anvil {

const char* snap_label(SnapType t) {
    switch (t) {
        case SnapType::Vertex: return "Vertex";
        case SnapType::Origin: return "Origin";
        case SnapType::Intersection: return "Intersection";
        case SnapType::Midpoint: return "Midpoint";
        case SnapType::Center: return "Center";
        case SnapType::Centroid: return "Centroid";
        case SnapType::Tangent: return "Tangent";
        case SnapType::Perpendicular: return "Perpendicular";
        case SnapType::Axis: return "Axis";
        case SnapType::Normal: return "Surface";
        case SnapType::Grid: return "Grid";
        case SnapType::Plane: return "Plane";
        default: return "";
    }
}

SnapSettings::SnapSettings() {
    auto set = [&](SnapType t, double r, double b) { types[int(t)] = {true, r, b}; };
    set(SnapType::Vertex, 14, 0.00);
    set(SnapType::Origin, 14, 0.00);
    set(SnapType::Intersection, 14, 0.05);
    set(SnapType::Midpoint, 14, 0.10);
    set(SnapType::Center, 16, 0.10);
    set(SnapType::Centroid, 14, 0.15);
    set(SnapType::Tangent, 12, 0.20);
    set(SnapType::Perpendicular, 12, 0.20);
    set(SnapType::Axis, 10, 0.35);
    set(SnapType::Normal, 1e9, 0.80);  // always available on a surface, lowest priority but grid
    set(SnapType::Grid, 10, 0.90);
    set(SnapType::Plane, 1e9, 1.00);   // fallback: free point on the work plane
}

std::shared_ptr<SnapObject> make_snap_object(std::string name, std::shared_ptr<const Mesh> mesh,
                                             AnalyticFeatures af) {
    auto o = std::make_shared<SnapObject>();
    o->name = std::move(name);
    o->mesh = mesh;
    o->bvh = std::make_shared<Bvh>(*mesh);
    const std::uint64_t N = mesh->V.size();
    for (const auto& e : feature_edges(*mesh, tol::kFeatureEdgeAngleRad)) o->feature_keys.insert(e[0] * N + e[1]);
    o->analytic = std::move(af);
    o->centroid = mesh->centroid();
    return o;
}

const std::array<Vec3, 26>& cube_directions() {
    static const std::array<Vec3, 26> d = [] {
        std::array<Vec3, 26> a{};
        int k = 0;
        for (int x = -1; x <= 1; ++x)
            for (int y = -1; y <= 1; ++y)
                for (int z = -1; z <= 1; ++z)
                    if (x || y || z) a[k++] = normalized(Vec3{double(x), double(y), double(z)});
        return a;
    }();
    return d;
}

std::array<int, 3> classify_cube_hit(const Vec3& p, double f) {
    // The hit lies on the face whose |coordinate| is maximal. The two other
    // coordinates are in [-1,1]; values beyond 1-2f (the outer band) select the
    // adjacent edge/corner, so the face is split 3x3: corner|edge|center.
    double a[3] = {p.x, p.y, p.z};
    int face = 0;
    for (int i = 1; i < 3; ++i)
        if (std::abs(a[i]) > std::abs(a[face])) face = i;
    std::array<int, 3> d{0, 0, 0};
    d[face] = a[face] > 0 ? 1 : -1;
    for (int i = 0; i < 3; ++i)
        if (i != face && std::abs(a[i]) > 1.0 - 2.0 * f) d[i] = a[i] > 0 ? 1 : -1;
    return d;
}

namespace {

// Closest point on line (p, d) to ray (o, r): returns parameter t along the line.
double line_ray_closest(const Vec3& p, const Vec3& d, const Vec3& o, const Vec3& r) {
    Vec3 w = p - o;
    double a = dot(d, d), b = dot(d, r), c = dot(r, r), dd = dot(d, w), e = dot(r, w);
    double den = a * c - b * b;
    if (std::abs(den) < 1e-15 * a * c) return 0.0;  // parallel
    return (b * e - c * dd) / den;
}

struct Cand {
    SnapType type;
    Vec3 p;
    std::string label;
    Vec3 axis{};
};

}  // namespace

SnapResult SnapEngine::update(const SnapScene& scene, const SnapQuery& q) {
    const Camera& cam = q.cam;
    Ray ray = cam.ray(q.mx, q.my);

    // 1. surface under the cursor
    std::optional<RayHit> hit;
    const SnapObject* hit_obj = nullptr;
    for (const auto& o : scene.objects) {
        if (o->name == q.ignore_object) continue;
        double tmax = hit ? hit->t : INFINITY;
        if (auto h = o->bvh->raycast(ray.origin, ray.dir, 0.0, tmax)) hit = h, hit_obj = o.get();
    }

    // work plane z = 0 (M1); intersection used for grid and free placement
    std::optional<Vec3> plane_pt;
    if (std::abs(ray.dir.z) > 1e-12) {
        double t = -ray.origin.z / ray.dir.z;
        if (t > 0) plane_pt = ray.origin + ray.dir * t;
    }

    SnapResult raw;
    if (hit) {
        raw.type = SnapType::Normal;
        raw.point = hit->point;
        raw.normal = hit->normal;
        raw.object = hit_obj->name;
    } else if (plane_pt) {
        raw.type = SnapType::Plane;
        raw.point = *plane_pt;
    } else {
        raw.type = SnapType::None;
        raw.point = cam.target;
    }
    raw.label = snap_label(raw.type);
    cam.project(raw.point, raw.px, raw.py);
    if (q.disabled) {
        last_.reset();
        return raw;
    }

    std::vector<Cand> cands;
    const double dpi = settings.dpi_scale;
    double depth = hit ? dot(hit->point - cam.eye(), cam.forward())
                       : (plane_pt ? dot(*plane_pt - cam.eye(), cam.forward()) : cam.distance);
    double wpp = cam.world_per_pixel(depth);
    double rmax_px = 0;
    for (const auto& t : settings.types)
        if (t.radius_px < 1e8) rmax_px = std::max(rmax_px, t.radius_px * dpi);

    cands.push_back({SnapType::Origin, {0, 0, 0}, "Origin"});
    for (const auto& o : scene.objects) {
        if (o->name == q.ignore_object) continue;
        for (const auto& c : o->analytic.centers) cands.push_back({SnapType::Center, c, "Center"});
        for (const auto& c : o->analytic.circles) cands.push_back({SnapType::Center, c.center, "Center"});
        if (std::isfinite(o->centroid.x) && std::isfinite(o->centroid.y) && std::isfinite(o->centroid.z))
            cands.push_back({SnapType::Centroid, o->centroid, "Centroid (" + o->name + ")"});
        // tangent points of circles seen from the base point
        if (q.base) {
            for (const auto& c : o->analytic.circles) {
                Vec3 b = *q.base - c.normal * dot(*q.base - c.center, c.normal);  // base projected into plane
                Vec3 u = b - c.center;
                double d = norm(u);
                if (d <= c.radius * (1 + 1e-9)) continue;
                u = u / d;
                Vec3 v = cross(c.normal, u);
                double ca = c.radius / d, sa = std::sqrt(1 - ca * ca);
                for (double s : {1.0, -1.0})
                    cands.push_back({SnapType::Tangent, c.center + (u * ca + v * (s * sa)) * c.radius, "Tangent"});
            }
        }
    }

    // 2. mesh features near the cursor. If the center ray misses (cursor just
    // outside a silhouette), probe a ring of rays so corners stay snappable.
    std::optional<RayHit> probe = hit;
    if (!probe) {
        double best_d = INFINITY;
        for (double rr : {0.5, 1.0})
            for (int k = 0; k < 16; ++k) {
                double a = k * 0.39269908169872414, ox = std::cos(a) * rr * rmax_px, oy = std::sin(a) * rr * rmax_px;
                Ray pr = cam.ray(q.mx + ox, q.my + oy);
                for (const auto& o : scene.objects) {
                    if (o->name == q.ignore_object) continue;
                    if (auto h = o->bvh->raycast(pr.origin, pr.dir)) {
                        if (std::hypot(ox, oy) < best_d) best_d = std::hypot(ox, oy), probe = h;
                    }
                }
            }
        if (probe) wpp = cam.world_per_pixel(dot(probe->point - cam.eye(), cam.forward()));
    }
    if (probe) {
        double rho = rmax_px * wpp;
        Box3 box;
        box.add(probe->point - Vec3{rho, rho, rho});
        box.add(probe->point + Vec3{rho, rho, rho});
        std::vector<std::array<Vec3, 2>> near_edges;
        for (const auto& o : scene.objects) {
            if (o->name == q.ignore_object || !o->bvh->bounds().overlaps(box)) continue;
            const std::uint64_t N = o->mesh->V.size();
            std::unordered_set<std::uint64_t> seen;
            o->bvh->query(box, [&](std::uint32_t f) {
                const auto& t = o->mesh->F[f];
                for (int k = 0; k < 3; ++k) {
                    std::uint32_t a = t[k], b = t[(k + 1) % 3];
                    std::uint64_t key = std::uint64_t(std::min(a, b)) * N + std::max(a, b);
                    if (!o->feature_keys.count(key) || !seen.insert(key).second) continue;
                    const Vec3 &pa = o->mesh->V[a], &pb = o->mesh->V[b];
                    cands.push_back({SnapType::Vertex, pa, "Vertex"});
                    cands.push_back({SnapType::Vertex, pb, "Vertex"});
                    cands.push_back({SnapType::Midpoint, (pa + pb) * 0.5, "Midpoint"});
                    if (q.base) {
                        Vec3 d = pb - pa;
                        double tt = dot(*q.base - pa, d) / dot(d, d);
                        if (tt > 0 && tt < 1) cands.push_back({SnapType::Perpendicular, pa + d * tt, "Perpendicular"});
                    }
                    if (near_edges.size() < 64) near_edges.push_back({pa, pb});
                }
            });
        }
        // intersections of nearby feature edges that do not share an endpoint
        for (std::size_t i = 0; i < near_edges.size(); ++i)
            for (std::size_t j = i + 1; j < near_edges.size(); ++j) {
                const auto &e1 = near_edges[i], &e2 = near_edges[j];
                if (e1[0] == e2[0] || e1[0] == e2[1] || e1[1] == e2[0] || e1[1] == e2[1]) continue;
                Vec3 d1 = e1[1] - e1[0], d2 = e2[1] - e2[0], r = e1[0] - e2[0];
                double a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r), c = dot(d1, r), b = dot(d1, d2);
                double den = a * e - b * b;
                if (den <= 1e-12 * a * e) continue;
                double s = (b * f - c * e) / den, t = (a * f - b * c) / den;
                if (s <= 0 || s >= 1 || t <= 0 || t >= 1) continue;
                Vec3 p1 = e1[0] + d1 * s, p2 = e2[0] + d2 * t;
                if (norm(p1 - p2) < std::max(tol::kConfusionMm, wpp * 0.5))
                    cands.push_back({SnapType::Intersection, (p1 + p2) * 0.5, "Intersection"});
            }
        if (hit) cands.push_back({SnapType::Normal, hit->point, "Surface"});
    }

    // 3. axes: through the base point (26 directions) or the origin (X/Y/Z)
    {
        static const char* names[3] = {"X axis", "Y axis", "Z axis"};
        if (q.base) {
            for (const auto& d : cube_directions()) {
                double t = line_ray_closest(*q.base, d, ray.origin, ray.dir);
                if (t <= 0) continue;
                char lbl[64];
                int ax = -1;
                if (std::abs(d.x) == 1 || std::abs(d.y) == 1 || std::abs(d.z) == 1)
                    ax = std::abs(d.x) == 1 ? 0 : std::abs(d.y) == 1 ? 1 : 2;
                if (ax >= 0) std::snprintf(lbl, sizeof lbl, "%s", names[ax]);
                else std::snprintf(lbl, sizeof lbl, "Direction (%+.0f,%+.0f,%+.0f)", std::round(d.x * 1.8),
                                   std::round(d.y * 1.8), std::round(d.z * 1.8));
                cands.push_back({SnapType::Axis, *q.base + d * t, lbl, d});
            }
        } else {
            for (int a = 0; a < 3; ++a) {
                Vec3 d{double(a == 0), double(a == 1), double(a == 2)};
                double t = line_ray_closest({0, 0, 0}, d, ray.origin, ray.dir);
                cands.push_back({SnapType::Axis, d * t, names[a], d});
            }
        }
    }

    // 4. grid on the work plane (only when nothing is hit)
    if (!hit && plane_pt) {
        double g = settings.grid_mm;
        // adaptive: at least ~8 px between grid points
        while (g / wpp < 8) g *= 10;
        Vec3 gp{std::round(plane_pt->x / g) * g, std::round(plane_pt->y / g) * g, 0};
        char lbl[64];
        std::snprintf(lbl, sizeof lbl, "Grid %g mm", g);
        cands.push_back({SnapType::Grid, gp, lbl});
        cands.push_back({SnapType::Plane, *plane_pt, "Plane"});
    }

    // 5. score
    std::optional<SnapResult> best;
    auto score = [&](const Cand& c, SnapResult& out) -> bool {
        const auto& ts = settings.types[int(c.type)];
        if (!ts.enabled) return false;
        double px, py, dep;
        if (!cam.project(c.p, px, py, &dep)) return false;
        double dist = std::hypot(px - q.mx, py - q.my);
        double r = ts.radius_px * dpi;
        if (dist > r) return false;
        // occlusion: a feature behind the hit surface is not snappable
        if (hit && c.type != SnapType::Axis &&
            dep > dot(hit->point - cam.eye(), cam.forward()) + 4 * wpp + tol::kConfusionMm)
            return false;
        out.type = c.type;
        out.point = c.p;
        out.px = px, out.py = py;
        out.score = (r < 1e8 ? dist / r : 0.0) + ts.beta;
        out.label = c.label;
        out.axis_dir = c.axis;
        out.normal = hit ? hit->normal : Vec3{0, 0, 1};
        out.object = hit_obj ? hit_obj->name : "";
        return true;
    };
    for (const auto& c : cands) {
        SnapResult r;
        if (score(c, r) && (!best || r.score < best->score)) best = r;
    }
    if (!best) {
        last_.reset();
        return raw;
    }
    // 6. hysteresis: keep the previous winner unless clearly beaten
    if (last_) {
        for (const auto& c : cands) {
            if (c.type != last_->type || norm(c.p - last_->point) > tol::kConfusionMm) continue;
            SnapResult prev;
            if (score(c, prev) && !(best->score < prev.score - settings.hysteresis)) best = prev;
            break;
        }
    }
    last_ = best;
    return *best;
}

}  // namespace anvil
