#include "anvil/figure.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace anvil::figure {

namespace {
// Rodrigues rotation of v about the unit axis k by angle a.
Vec3 rot(const Vec3& v, const Vec3& k, double a) {
    const double c = std::cos(a), s = std::sin(a);
    return v * c + cross(k, v) * s + k * (dot(k, v) * (1 - c));
}
// 3x3 rotation as a function object composed from axis-angle steps
struct Frame {
    Vec3 x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
    Vec3 apply(const Vec3& v) const { return x * v.x + y * v.y + z * v.z; }
    Frame rotated(const Vec3& local_axis, double a) const {  // rotate about an axis given in this frame
        const Vec3 k = normalized(apply(local_axis));
        Frame f;
        f.x = rot(x, k, a), f.y = rot(y, k, a), f.z = rot(z, k, a);
        return f;
    }
};

double sd_triangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {  // unsigned (Quilez)
    const Vec3 ba = b - a, pa = p - a, cb = c - b, pb = p - b, ac = a - c, pc = p - c;
    const Vec3 nor = cross(ba, ac);
    auto sgn = [](double v) { return v < 0 ? -1.0 : 1.0; };
    auto dot2 = [](const Vec3& v) { return dot(v, v); };
    const double inside = sgn(dot(cross(ba, nor), pa)) + sgn(dot(cross(cb, nor), pb)) + sgn(dot(cross(ac, nor), pc));
    double d2;
    if (inside < 2.0) {
        auto seg = [&](const Vec3& e, const Vec3& q) { return dot2(e * std::clamp(dot(e, q) / dot2(e), 0.0, 1.0) - q); };
        d2 = std::min({seg(ba, pa), seg(cb, pb), seg(ac, pc)});
    } else {
        d2 = dot(nor, pa) * dot(nor, pa) / dot2(nor);
    }
    return std::sqrt(d2);
}
}  // namespace

const std::vector<std::string>& pose_names() {
    static const std::vector<std::string> n{
        "spine", "neck", "head_turn", "head_tilt",
        "l_shoulder_raise", "l_shoulder_fwd", "l_elbow", "r_shoulder_raise", "r_shoulder_fwd", "r_elbow",
        "l_hip_fwd", "l_hip_side", "l_knee", "r_hip_fwd", "r_hip_side", "r_knee",
        "tail_curl", "wing_spread", "horn_curl"};
    return n;
}

double sd_round_cone(const Vec3& p, const RoundCone& c) {
    // round cone between spheres (a, ra) and (b, rb); Quilez, exact
    const Vec3 ba = c.b - c.a;
    const double l2 = dot(ba, ba);
    if (l2 < 1e-18) return norm(p - c.a) - std::max(c.ra, c.rb);
    const double rr = c.ra - c.rb, a2 = l2 - rr * rr, il2 = 1.0 / l2;
    const Vec3 pa = p - c.a;
    const double y = dot(pa, ba), z = y - l2;
    const Vec3 xv = pa * l2 - ba * y;
    const double x2 = dot(xv, xv), y2 = y * y * l2, z2 = z * z * l2;
    const double k = (rr < 0 ? -1.0 : 1.0) * rr * rr * x2;
    if ((z < 0 ? -1.0 : 1.0) * a2 * z2 > k) return std::sqrt(x2 + z2) * il2 - c.rb;
    if ((y < 0 ? -1.0 : 1.0) * a2 * y2 < k) return std::sqrt(x2 + y2) * il2 - c.ra;
    return (std::sqrt(x2 * a2 * il2) + y * rr) * il2 - c.ra;
}

double Body::sdf(const Vec3& p) const {
    double d = 1e30;
    for (const auto& c : cones) {
        // cheap bound first: distance to the cone's bounding sphere
        const Vec3 m = (c.a + c.b) * 0.5;
        const double br = 0.5 * norm(c.b - c.a) + std::max(c.ra, c.rb);
        const double lb = norm(p - m) - br;
        if (lb > d + blend) continue;  // cannot change the smooth minimum
        d = smin(d, sd_round_cone(p, c), blend);
    }
    for (const auto& s : slabs) d = smin(d, sd_triangle(p, s.a, s.b, s.c) - s.t, blend * 0.5);
    return d;
}

Body make_body(const Params& P) {
    const bool demon = P.preset == "demon";
    auto ang = [&](const char* n) {
        auto it = P.pose.find(n);
        return it == P.pose.end() ? 0.0 : it->second;
    };
    const double bf = 0.85 + 0.4 * std::clamp(P.build, 0.0, 1.0);       // girth
    const double hr = std::clamp(P.heroic, 0.0, 1.0);
    const double head_s = 1.0 + 0.25 * hr + (demon ? 0.12 : 0.0), ext_s = 1.0 + 0.3 * hr, limb_s = 1.0 + 0.1 * hr + (demon ? 0.12 : 0.0);
    const double dm = demon ? 1.28 : 1.0;  // demons: broader, more massive

    std::vector<RoundCone> C;
    std::vector<Slab> S;
    auto cone = [&](Vec3 a, Vec3 b, double ra, double rb) { C.push_back({a, b, ra, rb}); };
    auto ball = [&](Vec3 a, double r) { C.push_back({a, a, r, r}); };

    // ---- spine (real millimetres, figure faces -Y, left = +X)
    const Vec3 pelvis{0, 0, 950}, waist{0, 0, 1080};
    const Frame body;
    const Frame chest_f = body.rotated({1, 0, 0}, ang("spine"));         // forward bend about X
    const Vec3 chest = waist + chest_f.apply({0, 0, 250});
    const Vec3 neck_base = chest + chest_f.apply({0, 0, 175});
    const Frame neck_f = chest_f.rotated({1, 0, 0}, ang("neck"));
    const Frame head_f = neck_f.rotated({0, 0, 1}, ang("head_turn")).rotated({1, 0, 0}, ang("head_tilt"));
    const Vec3 head = neck_base + neck_f.apply({0, -8, 95}) + head_f.apply({0, 0, 48});

    // torso
    cone(pelvis, waist, 150 * bf * dm, 120 * bf * dm);
    cone(waist, chest, 120 * bf * dm, 160 * bf * dm);
    ball(chest + chest_f.apply({75, -55, 45}), 82 * bf * dm);   // pectorals
    ball(chest + chest_f.apply({-75, -55, 45}), 82 * bf * dm);
    ball(waist + chest_f.apply({0, -55, 0}), 95 * bf);          // belly
    ball(pelvis + Vec3{70, 55, -25}, 90 * bf * dm);              // glutes
    ball(pelvis + Vec3{-70, 55, -25}, 90 * bf * dm);
    cone(chest + chest_f.apply({190, 0, 120}) * 1.0, chest + chest_f.apply({-190, 0, 120}), 70 * bf * dm, 70 * bf * dm);
    // neck and head
    cone(neck_base, head - head_f.apply({0, 0, 50}), 58 * bf * dm, 50 * bf * dm);
    ball(head, 102 * head_s);
    cone(head + head_f.apply({0, -45, -25}) * head_s, head + head_f.apply({0, -75, -78}) * head_s, 62 * head_s, 42 * head_s);
    if (demon) {
        cone(head + head_f.apply({-55, -80, 35}) * head_s, head + head_f.apply({55, -80, 35}) * head_s, 24 * head_s, 24 * head_s);  // brow
        // horns: curling arcs from the temples, tapering
        for (int side : {-1, 1}) {
            Vec3 p = head + head_f.apply({side * 62.0, -10, 70}) * head_s;
            Vec3 d = normalized(head_f.apply({side * 0.55, 0.25, 0.8}));
            const Vec3 axis = normalized(head_f.apply({1, 0, 0})) * double(side);
            const int n = 7;
            const double seg = 55 * head_s, r0 = 26 * head_s;
            for (int i = 0; i < n; ++i) {
                const Vec3 q = p + d * seg;
                cone(p, q, r0 * (1.0 - double(i) / n), r0 * (1.0 - double(i + 1) / n) + 2);
                d = normalized(rot(d, axis, (0.25 + ang("horn_curl")) / n * 1.6));
                p = q;
            }
        }
    }

    // ---- arms
    for (int side : {1, -1}) {
        const char* pre = side > 0 ? "l_" : "r_";
        auto A = [&](const char* n) { return ang((std::string(pre) + n).c_str()); };
        const Vec3 shoulder = chest + chest_f.apply({side * 195.0, 0, 120});
        // rest: hanging down, 10 degrees out; raise = abduction (about Y), fwd = flexion (about X)
        Frame f = chest_f.rotated({0, 1, 0}, -side * (0.17 + A("shoulder_raise"))).rotated({1, 0, 0}, -A("shoulder_fwd"));
        const Vec3 elbow = shoulder + f.apply({0, 0, -290});
        const Frame ff = f.rotated({1, 0, 0}, -A("elbow"));
        const Vec3 wrist = elbow + ff.apply({0, 0, -260});
        const Vec3 hand = wrist + ff.apply({0, -8, -85}) * ext_s;
        ball(shoulder + chest_f.apply({side * 10.0, 0, 5}), 72 * bf * dm * limb_s);          // deltoid
        cone(shoulder, elbow, 52 * bf * dm * limb_s, 40 * bf * limb_s);
        ball(shoulder + f.apply({0, -18, -120}), 48 * bf * dm * limb_s);                       // biceps
        cone(elbow, wrist, 42 * bf * limb_s, 28 * bf * limb_s);
        cone(wrist, hand, 28 * ext_s, 38 * ext_s);
        if (demon)  // claws
            for (int k = -1; k <= 2; ++k) {
                const Vec3 base = hand + ff.apply({k * 14.0, -10, -25}) * ext_s;
                cone(base, base + ff.apply({k * 6.0, -30, -55}) * ext_s, 10 * ext_s, 1.5);
            }
    }

    // ---- legs
    for (int side : {1, -1}) {
        const char* pre = side > 0 ? "l_" : "r_";
        auto A = [&](const char* n) { return ang((std::string(pre) + n).c_str()); };
        const Vec3 hip = pelvis + Vec3{side * 95.0, 0, -30};
        Frame f = body.rotated({1, 0, 0}, -A("hip_fwd")).rotated({0, 1, 0}, -side * A("hip_side"));
        const Vec3 knee = hip + f.apply({0, 0, -440});
        cone(hip, knee, 85 * bf * dm * limb_s, 58 * bf * dm * limb_s);
        ball(hip + f.apply({0, -40, -150}), 70 * bf * dm * limb_s);  // quadriceps
        if (!demon) {
            const Frame sf = f.rotated({1, 0, 0}, A("knee"));
            const Vec3 ankle = knee + sf.apply({0, 0, -420});
            cone(knee, ankle, 55 * bf * limb_s, 35 * bf * limb_s);
            ball(knee + sf.apply({0, 45, -120}), 50 * bf * limb_s);  // calf
            const Vec3 toe = ankle + Vec3{0, -200, -55} * ext_s;
            cone(ankle, toe, 38 * ext_s, 30 * ext_s);
        } else {
            // digitigrade: shin back, metatarsus forward-down to the ball of the foot
            const Frame sf = f.rotated({1, 0, 0}, 0.55 + A("knee"));
            const Vec3 hock = knee + sf.apply({0, 0, -330});
            const Frame mf = sf.rotated({1, 0, 0}, -0.9);
            const Vec3 ball_p = hock + mf.apply({0, 0, -280});
            cone(knee, hock, 68 * bf * dm * limb_s, 46 * bf * dm * limb_s);
            ball(knee + sf.apply({0, 45, -110}), 62 * bf * dm * limb_s);  // calf
            cone(hock, ball_p, 44 * bf * dm, 36 * ext_s);
            for (int k = -1; k <= 1; ++k) {
                const Vec3 b2 = ball_p + Vec3{k * 26.0, -40, -5} * ext_s;
                cone(b2, b2 + Vec3{k * 10.0, -80, -15} * ext_s, 16 * ext_s, 2);
            }
        }
    }

    if (demon) {
        // ---- tail: tapering chain from the sacrum, curling by tail_curl
        Vec3 p = pelvis + Vec3{0, 95, -20};
        Vec3 d = normalized(Vec3{0, 0.75, -0.66});
        const int n = 10;
        for (int i = 0; i < n; ++i) {
            const Vec3 q = p + d * 95;
            cone(p, q, 46 * bf * (1.0 - 0.85 * i / n), 46 * bf * (1.0 - 0.85 * (i + 1) / n));
            // rotating about +X turns the back-pointing tail upwards (+Y towards +Z)
            d = normalized(rot(d, {1, 0, 0}, ang("tail_curl") / n - 0.03));
            p = q;
        }
        cone(p, p + d * 70, 22, 2);  // spade
        // ---- wings: arm bone up-out-back, three fingers, membrane slabs
        for (int side : {1, -1}) {
            const Vec3 root = chest + chest_f.apply({side * 110.0, 95, 70});
            const double spread = 0.6 + ang("wing_spread");
            Frame wf = chest_f.rotated({0, 1, 0}, -side * spread).rotated({1, 0, 0}, 0.45);
            const Vec3 wel = root + wf.apply({0, 0, 420});
            cone(root, wel, 30 * bf, 20);
            Vec3 tips[3];
            for (int k = 0; k < 3; ++k) {
                const double fan = -0.25 + 0.65 * k;
                const Frame tf = wf.rotated({0, 1, 0}, -side * fan).rotated({1, 0, 0}, -0.3 - 0.35 * k);
                tips[k] = wel + tf.apply({0, 0, 600 - 70.0 * k});
                cone(wel, tips[k], 15, 4);
            }
            const double t = 6;
            S.push_back({root, wel, tips[0], t});
            S.push_back({root, tips[0], tips[1], t});
            S.push_back({root, tips[1], tips[2], t});
        }
    }

    // ---- stand on z = 0 and scale to the target height (top of the head in the rest pose)
    double top_real;
    {
        // rest head top: same construction with zero pose (spine/neck/head only)
        const Vec3 c0 = waist + Vec3{0, 0, 250}, nb0 = c0 + Vec3{0, 0, 175};
        const Vec3 h0 = nb0 + Vec3{0, -8, 95} + Vec3{0, 0, 48};
        top_real = h0.z + 102 * head_s;
    }
    // lowest point of the legs and feet (the posed figure stands on the ground)
    double ground = 1e30;
    for (const auto& c : C) ground = std::min({ground, c.a.z - c.ra, c.b.z - c.rb});
    const Vec3 lift{0, 0, -ground};
    const double s = P.height / (top_real - ground);
    const double rmin = P.min_feature;
    Body b;
    b.blend = 35 * s;
    for (auto c : C) {
        c.a = (c.a + lift) * s, c.b = (c.b + lift) * s;
        c.ra = std::max(c.ra * s, rmin), c.rb = std::max(c.rb * s, rmin);
        b.bounds.add(c.a - Vec3{c.ra, c.ra, c.ra});
        b.bounds.add(c.a + Vec3{c.ra, c.ra, c.ra});
        b.bounds.add(c.b - Vec3{c.rb, c.rb, c.rb});
        b.bounds.add(c.b + Vec3{c.rb, c.rb, c.rb});
        b.cones.push_back(c);
    }
    for (auto sl : S) {
        sl.a = (sl.a + lift) * s, sl.b = (sl.b + lift) * s, sl.c = (sl.c + lift) * s, sl.t = std::max(sl.t * s, rmin);
        for (const auto& v : {sl.a, sl.b, sl.c}) b.bounds.add(v - Vec3{sl.t, sl.t, sl.t}), b.bounds.add(v + Vec3{sl.t, sl.t, sl.t});
        b.slabs.push_back(sl);
    }
    return b;
}

// ---------------------------------------------------------------- bases
namespace {
// integer hash -> [0, 1) (Wang/xorshift mix): reproducible stone layouts
double hash01(int i, int j, unsigned seed, int salt) {
    std::uint32_t x = std::uint32_t(i) * 73856093u ^ std::uint32_t(j) * 19349663u ^ (seed + 1u) * 83492791u ^
                      std::uint32_t(salt) * 2654435761u;
    x ^= x >> 16, x *= 0x7feb352du, x ^= x >> 15, x *= 0x846ca68bu, x ^= x >> 16;
    return (x & 0xffffff) / double(0x1000000);
}
}  // namespace

Box3 Base::bounds() const {
    Box3 b;
    b.add(Vec3{-d / 2, -d / 2, -h});
    b.add(Vec3{d / 2, d / 2, texture == "rubble" ? 0.25 * h : 0.05 * h});
    return b;
}

double Base::sdf(const Vec3& p) const {
    const double R = d / 2, bevel = std::min(0.45 * h, 0.06 * d);
    // truncated cone (45-degree bevel at the top edge) between z = -h and z = 0:
    // radius R below z = -bevel, shrinking 1:1 above; distances to the side are
    // scaled by cos 45 deg on the bevel so the field stays 1-Lipschitz
    const double rad = std::hypot(p.x, p.y);
    const double side = rad - R;
    const double bev = (rad + p.z + bevel - R) * std::numbers::sqrt2 / 2;
    double f = std::max({side, bev, -h - p.z});
    if (texture == "plain") return std::max(f, p.z);
    if (texture == "flagstone") {
        // jittered-grid Voronoi stones (cell ~ d/6); groove along the cell edges
        const double cell = d / 6, w = std::max(0.06 * cell, min_feature), depth = 0.22 * h;
        const int ci = int(std::floor(p.x / cell)), cj = int(std::floor(p.y / cell));
        double d1 = 1e9, d2 = 1e9;
        int bi = 0, bj = 0;
        for (int dj = -1; dj <= 1; ++dj)
            for (int di = -1; di <= 1; ++di) {
                const int i = ci + di, j = cj + dj;
                const double sx = (i + 0.15 + 0.7 * hash01(i, j, seed, 1)) * cell;
                const double sy = (j + 0.15 + 0.7 * hash01(i, j, seed, 2)) * cell;
                const double dd = std::hypot(p.x - sx, p.y - sy);
                if (dd < d1) d2 = d1, d1 = dd, bi = i, bj = j;
                else if (dd < d2) d2 = dd;
            }
        const double edge = 0.5 * (d2 - d1);                   // lower bound of the distance to the cell edge
        // stones of slightly different height, blending to 0 towards the joint
        // (no jump in f across a cell edge: the field stays continuous)
        const double ramp = std::clamp((edge - w) / (2 * w), 0.0, 1.0);
        const double top = 0.06 * h * hash01(bi, bj, seed, 3) * ramp;
        f = std::max(f, p.z - top);
        // groove volume; the joints get shallower towards the rim and vanish before
        // the bevel (a hard end wall makes a sharp three-plane corner that folds)
        const double fade = std::clamp((R - bevel - 3 * w - rad) / (0.5 * cell), 0.0, 1.0);
        const double groove = std::max(edge - w, -(p.z + depth * fade - 1e-3 * (1 - fade)));
        return std::max(f, -groove);
    }
    // rubble: flat top with scattered stones
    f = std::max(f, p.z);
    const double cell = d / 5;
    const int ci = int(std::floor(p.x / cell)), cj = int(std::floor(p.y / cell));
    for (int dj = -1; dj <= 1; ++dj)
        for (int di = -1; di <= 1; ++di) {
            const int i = ci + di, j = cj + dj;
            for (int k = 0; k < 2; ++k) {
                if (hash01(i, j, seed, 10 + k) < 0.35) continue;
                const double r = (0.06 + 0.07 * hash01(i, j, seed, 20 + k)) * cell * 2;
                const Vec3 c{(i + hash01(i, j, seed, 30 + k)) * cell, (j + hash01(i, j, seed, 40 + k)) * cell, -0.3 * r};
                if (std::hypot(c.x, c.y) > R - bevel - r) continue;
                f = smin(f, norm((p - c) * 1.0) - r, 0.3 * r);
            }
        }
    return f;
}

}  // namespace anvil::figure
