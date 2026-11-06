// Cut regions (cut.hpp).
#include "anvil/cut.hpp"

#include <clipper2/clipper.h>

#include <cmath>
#include <sstream>
#include <stdexcept>

namespace anvil::cut {

namespace {
std::vector<double> numbers(const std::string& s) {
    std::vector<double> out;
    std::string tok;
    for (char c : s + ",") {
        if (c == ',' || c == ';' || c == ' ') {
            if (!tok.empty()) {
                std::size_t used = 0;
                double v = std::stod(tok, &used);
                if (used != tok.size() || !std::isfinite(v)) throw std::runtime_error("cut: bad number '" + tok + "'");
                out.push_back(v);
            }
            tok.clear();
        } else {
            tok.push_back(c);
        }
    }
    return out;
}
constexpr int kDecimals = 6;  // Clipper2 precision: 1 nm in mm units
}  // namespace

Spec parse(const std::map<std::string, std::string>& attrs) {
    Spec s;
    auto get = [&](const char* k, const char* def) {
        auto it = attrs.find(k);
        return it == attrs.end() ? std::string(def) : it->second;
    };
    s.mode = get("mode", "knife");
    s.keep = get("keep", s.mode == "knife" ? "left" : s.mode == "lasso" ? "outside" : "above");
    if (s.mode != "knife" && s.mode != "lasso" && s.mode != "plane")
        throw std::runtime_error("cut: mode must be 'knife', 'lasso' or 'plane', got '" + s.mode + "'");
    const bool ok = (s.mode == "knife" && (s.keep == "left" || s.keep == "right")) ||
                    (s.mode == "lasso" && (s.keep == "inside" || s.keep == "outside")) ||
                    (s.mode == "plane" && (s.keep == "above" || s.keep == "below"));
    if (!ok) throw std::runtime_error("cut: keep='" + s.keep + "' does not fit mode '" + s.mode + "'");
    const auto p = numbers(get("path", ""));
    if (p.size() % 3) throw std::runtime_error("cut: path needs x, y, z per point");
    for (std::size_t i = 0; i + 2 < p.size(); i += 3) s.path.push_back({p[i], p[i + 1], p[i + 2]});
    const auto d = numbers(get("dir", "0,0,-1"));
    if (d.size() != 3 || norm(Vec3{d[0], d[1], d[2]}) < 1e-12) throw std::runtime_error("cut: direction must be a non-zero vector");
    s.dir = normalized(Vec3{d[0], d[1], d[2]});
    if (const auto e = numbers(get("eye", "")); !e.empty()) {
        if (e.size() != 3) throw std::runtime_error("cut: eye needs x, y, z");
        s.has_eye = true;
        s.eye = {e[0], e[1], e[2]};
        if (dot(s.path.empty() ? Vec3{} : s.path.front() - s.eye, s.dir) <= 0)
            throw std::runtime_error("cut: the path lies behind the eye");
    }
    const std::size_t need = s.mode == "knife" ? 2 : s.mode == "lasso" ? 3 : 1;
    if (s.path.size() < need)
        throw std::runtime_error("cut: a " + s.mode + " cut needs at least " + std::to_string(need) + " points");
    return s;
}

bool keeps_region(const Spec& s) { return s.keep == "left" || s.keep == "inside" || s.keep == "above"; }

Frame frame(const Spec& s) {
    Frame f;
    f.o = s.path.front();
    f.n = s.dir;
    // u: the axis least aligned with n, made orthogonal; v = n x u
    Vec3 a = std::abs(f.n.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    f.u = normalized(a - f.n * dot(a, f.n));
    f.v = cross(f.n, f.u);
    if (s.has_eye && s.mode != "plane") {
        f.d0 = dot(s.path.front() - s.eye, f.n);
        f.o = s.eye + f.n * f.d0;  // foot of the eye on the path plane
    }
    return f;
}

std::vector<Polygon2> region(const Spec& s, double reach) {
    const Frame f = frame(s);
    std::vector<std::array<double, 2>> q;
    for (const auto& p : s.path) {
        std::array<double, 2> c{dot(p - f.o, f.u), dot(p - f.o, f.v)};
        if (q.empty() || std::hypot(c[0] - q.back()[0], c[1] - q.back()[1]) > 1e-9) q.push_back(c);
    }
    Clipper2Lib::PathD poly;
    if (s.mode == "plane") {
        // the whole plane (the cutter is then the half-space in front of it)
        poly = {{-reach, -reach}, {reach, -reach}, {reach, reach}, {-reach, reach}};
    } else if (s.mode == "lasso") {
        if (q.size() < 3) throw std::runtime_error("cut: the lasso path is degenerate (fewer than 3 distinct points)");
        for (const auto& c : q) poly.push_back({c[0], c[1]});
    } else {
        if (q.size() < 2) throw std::runtime_error("cut: the knife path is degenerate (its points coincide)");
        auto dir = [](std::array<double, 2> a, std::array<double, 2> b) {
            const double l = std::hypot(b[0] - a[0], b[1] - a[1]);
            return std::array<double, 2>{(b[0] - a[0]) / l, (b[1] - a[1]) / l};
        };
        const auto d0 = dir(q[0], q[1]), d1 = dir(q[q.size() - 2], q.back());
        const double L = 4 * reach;
        const std::array<double, 2> e0{q[0][0] - d0[0] * L, q[0][1] - d0[1] * L};
        const std::array<double, 2> e1{q.back()[0] + d1[0] * L, q.back()[1] + d1[1] * L};
        poly.push_back({e0[0], e0[1]});
        for (const auto& c : q) poly.push_back({c[0], c[1]});
        poly.push_back({e1[0], e1[1]});
        // close around the left side: (-d_y, d_x)
        poly.push_back({e1[0] - d1[1] * L, e1[1] + d1[0] * L});
        poly.push_back({e0[0] - d0[1] * L, e0[1] + d0[0] * L});
    }
    const Clipper2Lib::PathsD clean = Clipper2Lib::Union(Clipper2Lib::PathsD{poly}, Clipper2Lib::FillRule::NonZero, kDecimals);
    if (clean.empty()) throw std::runtime_error("cut: the path encloses no area");
    std::vector<Polygon2> out;
    for (const auto& path : clean) {
        Polygon2 p;
        for (const auto& pt : path) p.push_back({pt.x, pt.y});
        out.push_back(std::move(p));
    }
    return out;
}

bool in_region(const Spec& s, const Vec3& p, double reach) {
    const Frame f = frame(s);
    if (s.mode == "plane") return dot(p - f.o, f.n) > 0;
    Vec3 q3 = p;
    if (s.has_eye) {  // along the ray from the eye onto the path plane
        const double depth = dot(p - s.eye, f.n);
        if (depth <= 0) return false;
        q3 = s.eye + (p - s.eye) * (f.d0 / depth);
    }
    const auto reg = region(s, reach);
    Clipper2Lib::PathsD paths;
    for (const auto& r : reg) {
        Clipper2Lib::PathD pd;
        for (const auto& c : r) pd.push_back({c[0], c[1]});
        paths.push_back(pd);
    }
    // non-zero rule over the cleaned polygons (holes are oriented opposite)
    const Clipper2Lib::PointD q{dot(q3 - f.o, f.u), dot(q3 - f.o, f.v)};
    int winding = 0;
    for (const auto& pd : paths) {
        const auto r = Clipper2Lib::PointInPolygon(q, pd);
        if (r == Clipper2Lib::PointInPolygonResult::IsOutside) continue;
        winding += Clipper2Lib::IsPositive(pd) ? 1 : -1;
    }
    return winding != 0;
}

}  // namespace anvil::cut
