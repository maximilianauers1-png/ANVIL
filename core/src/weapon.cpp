// Parametric weapons (weapon.hpp). Proportions are those of the real weapons
// (longsword ~1.1 m with a 0.85 m blade, zweihander ~1.7 m, a "buster"
// fantasy greatsword with a 0.3 m wide blade, ...) exaggerated in width and
// thickness the way miniature sculptors do it so that they print at 28-32 mm
// scale (resin: blades >= 0.5 mm thick, edges >= 2 voxels).
#include "anvil/weapon.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace anvil::weapon {

Hull Hull::of(const std::vector<Vec3>& pts) {
    Hull h;
    for (const auto& p : pts) h.box.add(p);
    const double eps = 1e-9 * std::max(1.0, h.box.diagonal());
    const std::size_t n = pts.size();
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = i + 1; j < n; ++j)
            for (std::size_t k = j + 1; k < n; ++k) {
                Vec3 nn = cross(pts[j] - pts[i], pts[k] - pts[i]);
                const double l = norm(nn);
                if (l < eps) continue;
                nn = nn / l;
                double d = dot(nn, pts[i]), lo = 0, hi = 0;
                for (const auto& p : pts) {
                    const double s = dot(nn, p) - d;
                    lo = std::min(lo, s), hi = std::max(hi, s);
                }
                if (hi > eps && lo < -eps) continue;  // points on both sides: not a face
                if (hi > eps) nn = -nn, d = -d;       // orient outwards
                bool dup = false;
                for (const auto& q : h.planes)
                    if (std::abs(q[0] - nn.x) + std::abs(q[1] - nn.y) + std::abs(q[2] - nn.z) < 1e-9 && std::abs(q[3] - d) < eps)
                        dup = true;
                if (!dup) h.planes.push_back({nn.x, nn.y, nn.z, d});
            }
    if (h.planes.size() < 4) throw std::runtime_error("weapon: degenerate hull");
    return h;
}

double Hull::sdf(const Vec3& p) const {
    double f = -INFINITY;
    for (const auto& q : planes) f = std::max(f, q[0] * p.x + q[1] * p.y + q[2] * p.z - q[3]);
    return f;
}

double Weapon::sdf(const Vec3& p) const {
    double f = INFINITY;
    for (const auto& h : hulls) f = std::min(f, h.sdf(p));
    for (const auto& c : cones) f = std::min(f, figure::sd_round_cone(p, c));
    return f;
}

const std::vector<std::string>& types() {
    static const std::vector<std::string> t{"longsword", "zweihander", "buster", "dagger", "axe", "spear"};
    return t;
}

Params defaults(const std::string& type, double H) {
    // values for a 32 mm (heroic scale) figure, scaled linearly with its height
    struct D { const char* t; double L, W, T, G; };
    static const D table[] = {{"longsword", 19, 1.6, 0.6, 1.0}, {"zweihander", 30, 2.0, 0.7, 1.0},
                              {"buster", 30, 5.5, 1.2, 1.2},    {"dagger", 8, 1.3, 0.5, 0.9},
                              {"axe", 18, 6.0, 0.6, 1.0},       {"spear", 40, 2.2, 0.6, 0.9}};
    for (const auto& d : table)
        if (type == d.t) {
            const double k = H / 32.0;
            Params p;
            p.type = type, p.length = d.L * k, p.width = d.W * k, p.thickness = d.T * k, p.grip_d = d.G * k;
            return p;
        }
    throw std::runtime_error("weapon: unknown type '" + type + "'");
}

namespace {
// Blade cross-section at height z: a hexagon with flats of half thickness e at
// the edges (x = +-a) and the spine of half thickness b in the middle.
void section(std::vector<Vec3>& pts, double a, double b, double e, double z) {
    for (double sx : {-1.0, 1.0})
        for (double sy : {-1.0, 1.0}) pts.push_back({sx * a, sy * e, z});
    pts.push_back({0, b, z});
    pts.push_back({0, -b, z});
}
}  // namespace

Weapon make(const Params& p) {
    const double L = p.length, W = p.width, G = p.grip_d, mf = p.min_feature;
    const double T = std::max(p.thickness, 2.5 * mf);
    if (!(L > 0 && W > 0 && T > 0 && G > 0)) throw std::runtime_error("weapon: length, width, thickness and grip_d must be > 0");
    const double e = std::max(mf, 0.12 * T);  // edge flat (printable edge)
    const double b = T / 2;
    const double rg = std::max(G / 2, mf);
    Weapon w;
    auto cone = [&](Vec3 a, Vec3 c, double ra, double rb) { w.cones.push_back({a, c, std::max(ra, mf), std::max(rb, mf)}); };
    auto hull = [&](const std::vector<Vec3>& pts) { w.hulls.push_back(Hull::of(pts)); };
    const std::string& t = p.type;
    if (t == "longsword" || t == "zweihander" || t == "dagger" || t == "buster") {
        const double grip_frac = t == "zweihander" ? 0.13 : t == "dagger" ? 0.16 : t == "buster" ? 0.11 : 0.09;
        const double guard_frac = t == "zweihander" ? 0.15 : t == "dagger" ? 0.16 : t == "buster" ? 0.0 : 0.11;
        const double gh = grip_frac * L;  // grip half length
        cone({0, 0, -gh}, {0, 0, gh + 0.5 * (t == "buster" ? 0.9 * G : 0.45 * G)}, rg, 0.9 * rg);  // into the guard
        // pommel: a disc-like ball below the grip
        const double rp = std::max(0.7 * G, mf);
        cone({0, 0, -gh - 0.35 * G}, {0, 0, -gh - 0.35 * G}, rp, rp);
        const double gz = t == "buster" ? 0.9 * G : 0.45 * G;  // guard thickness along Z
        const double gx = t == "buster" ? W / 2 + 0.5 * G : guard_frac * L;
        const double gy = std::max(t == "buster" ? 0.8 * G : 0.5 * G, b + mf);
        hull({{-gx, -gy, gh}, {gx, -gy, gh}, {-gx, gy, gh}, {gx, gy, gh},
              {-gx, -gy, gh + gz}, {gx, -gy, gh + gz}, {-gx, gy, gh + gz}, {gx, gy, gh + gz}});
        // blade base sunk half way into the guard (parts overlap: one solid, no touching faces)
        const double z0 = gh + 0.5 * gz, z1 = L - gh - 0.35 * G - rp;
        if (z1 <= z0 + W) throw std::runtime_error("weapon: blade too short for its width (increase length)");
        const double a = W / 2;
        std::vector<Vec3> pts;
        if (t == "buster") {
            // broad flat blade with bevelled edges and a slanted, blunt top
            const double bev = 0.3 * a;
            for (double sy : {-1.0, 1.0}) {
                for (double sx : {-1.0, 1.0}) {
                    pts.push_back({sx * a, sy * e, z0});
                    pts.push_back({sx * (a - bev), sy * b, z0});
                    auto ztop = [&](double x) { return z1 - 0.9 * W * (x + a) / (2 * a); };
                    pts.push_back({sx * a, sy * e, ztop(sx * a)});
                    pts.push_back({sx * (a - bev), sy * b, ztop(sx * (a - bev))});
                }
            }
        } else {
            // straight double-edged blade: slight taper to the shoulder, then the point
            const double zs = z1 - (t == "dagger" ? 0.5 * (z1 - z0) : 1.5 * W);
            section(pts, a, b, e, z0);
            section(pts, 0.8 * a, 0.8 * b, e, zs);
            for (double sx : {-1.0, 1.0})
                for (double sy : {-1.0, 1.0}) pts.push_back({sx * mf, sy * mf, z1});
        }
        hull(pts);
    } else if (t == "axe") {
        cone({0, 0, -0.42 * L}, {0, 0, 0.58 * L}, rg, 0.9 * rg);
        const double zc = 0.48 * L;
        // head: from the socket around the haft to the curved edge on +X
        std::vector<Vec3> head;
        for (double sy : {-1.0, 1.0}) {
            head.push_back({-0.7 * G, sy * 0.6 * G, zc - 0.09 * L});
            head.push_back({-0.7 * G, sy * 0.6 * G, zc + 0.09 * L});
            head.push_back({0.06 * L, sy * std::max(0.45 * G, b), zc - 0.06 * L});
            head.push_back({0.06 * L, sy * std::max(0.45 * G, b), zc + 0.06 * L});
            for (int k = -2; k <= 2; ++k) {  // edge arc: 5 points
                const double ang = 0.35 * k;
                head.push_back({0.2 * L + 0.04 * L * std::cos(ang), sy * e, zc + W / 2 * std::sin(ang) / std::sin(0.7)});
            }
        }
        hull(head);
        hull({{-0.6 * G, -0.5 * G, zc - 0.05 * L}, {-0.6 * G, 0.5 * G, zc - 0.05 * L}, {-0.6 * G, -0.5 * G, zc + 0.05 * L},
              {-0.6 * G, 0.5 * G, zc + 0.05 * L}, {-0.13 * L, -mf, zc - mf}, {-0.13 * L, mf, zc + mf},
              {-0.13 * L, -mf, zc + mf}, {-0.13 * L, mf, zc - mf}});
    } else if (t == "spear") {
        cone({0, 0, -0.35 * L}, {0, 0, 0.53 * L}, rg, 0.85 * rg);
        std::vector<Vec3> pts;
        section(pts, std::max(0.5 * G, mf), std::max(0.5 * G, mf), std::max(0.5 * G, mf), 0.5 * L);
        section(pts, W / 2, b, e, 0.57 * L);
        for (double sx : {-1.0, 1.0})
            for (double sy : {-1.0, 1.0}) pts.push_back({sx * mf, sy * mf, 0.65 * L});
        hull(pts);
    } else {
        throw std::runtime_error("weapon: unknown type '" + t + "'");
    }
    for (const auto& h : w.hulls) w.bounds.add(h.box);
    for (const auto& c : w.cones) {
        const double r = std::max(c.ra, c.rb);
        w.bounds.add(c.a - Vec3{r, r, r}), w.bounds.add(c.a + Vec3{r, r, r});
        w.bounds.add(c.b - Vec3{r, r, r}), w.bounds.add(c.b + Vec3{r, r, r});
    }
    return w;
}

}  // namespace anvil::weapon
