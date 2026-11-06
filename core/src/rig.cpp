// Humanoid rig, forward kinematics and dual quaternion skinning (rig.hpp).
#include "anvil/rig.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <atomic>

#include "anvil/model_io.hpp"
#include "anvil/sdf.hpp"
#include <queue>
#include <unordered_map>
#include "anvil/winding.hpp"
#include "anvil/tolerance.hpp"

namespace anvil::rig {

// ------------------------------------------------------------------ quaternions
Vec3 Quat::rotate(const Vec3& v) const {
    // v' = q v q*: with u = (x, y, z), t = 2 u x v, v' = v + w t + u x t
    const Vec3 u{x, y, z};
    const Vec3 t = cross(u, v) * 2.0;
    return v + t * w + cross(u, t);
}
Quat Quat::axis_angle(const Vec3& axis, double angle) {
    const Vec3 a = normalized(axis);
    const double s = std::sin(angle / 2);
    return {std::cos(angle / 2), a.x * s, a.y * s, a.z * s};
}
Quat Quat::from_to(const Vec3& a_in, const Vec3& b_in) {
    const Vec3 a = normalized(a_in), b = normalized(b_in);
    const double d = dot(a, b);
    if (d < -1 + 1e-12) {  // opposite: 180 degrees about any perpendicular axis
        Vec3 axis = cross(a, {1, 0, 0});
        if (norm(axis) < 1e-6) axis = cross(a, {0, 1, 0});
        return axis_angle(axis, std::numbers::pi);
    }
    // half-way quaternion: (1 + a.b, a x b), normalised
    const Vec3 c = cross(a, b);
    Quat q{1 + d, c.x, c.y, c.z};
    const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    return {q.w / n, q.x / n, q.y / n, q.z / n};
}

namespace {

constexpr double kArmOut = 0.17;  // rad: rest arm 10 degrees out from the body (as figure.cpp)
// Separate shells up to a quarter of the figure's height (bounding-box
// diagonal) count as rigid parts: pauldrons, plates, a head or a hand; a whole
// separate arm or leg (~0.45 H) still bends at its joints.
constexpr double kRigidPartFraction = 0.25;
const Vec3 kDown{0, 0, -1};

Quat rx(double a) { return Quat::axis_angle({1, 0, 0}, a); }
Quat ry(double a) { return Quat::axis_angle({0, 1, 0}, a); }
Quat rz(double a) { return Quat::axis_angle({0, 0, 1}, a); }

template <class F>
void parallel_for(std::size_t n, F&& f) {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t nt = std::min<std::size_t>(hw, (n + 1023) / 1024);
    if (nt <= 1) {
        for (std::size_t i = 0; i < n; ++i) f(i);
        return;
    }
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> th;
    for (std::size_t t = 0; t < nt; ++t)
        th.emplace_back([&] {
            for (std::size_t i; (i = next.fetch_add(256)) < n;)
                for (std::size_t k = i; k < std::min(n, i + 256); ++k) f(k);
        });
    for (auto& t : th) t.join();
}

// ------------------------------------------------------------------ bone names
struct NameInfo {
    Role role = Role::None;
    int side = 0;
    bool middle_finger = false;
};

NameInfo classify(const std::string& raw) {
    NameInfo r;
    std::string n = raw;
    if (auto c = n.rfind(':'); c != std::string::npos) n = n.substr(c + 1);  // "mixamorig:LeftArm"
    // CamelCase side prefix "LUpperArm" / "RHand"
    if (n.size() > 2 && (n[0] == 'L' || n[0] == 'R') && std::isupper(static_cast<unsigned char>(n[1])) &&
        std::islower(static_cast<unsigned char>(n[2])))
        r.side = n[0] == 'L' ? 1 : -1, n = n.substr(1);
    std::string s;
    for (char ch : n) s.push_back(std::isalnum(static_cast<unsigned char>(ch)) ? char(std::tolower(static_cast<unsigned char>(ch))) : '_');
    for (const std::string pre : {"mixamorig_", "mixamorig", "def_", "org_", "bip01_", "bip001_", "bip_", "cc_base_"})
        if (s.rfind(pre, 0) == 0) s = s.substr(pre.size());
    auto erase = [&](const std::string& w) {
        auto p = s.find(w);
        if (p == std::string::npos) return false;
        s.erase(p, w.size());
        return true;
    };
    if (erase("left")) r.side = 1;
    else if (erase("right")) r.side = -1;
    // tokens "l" / "r" (upperarm_l, l_hand, bip01_l_thigh)
    std::vector<std::string> tok;
    {
        std::string cur;
        for (char ch : s + "_") {
            if (ch == '_') {
                if (!cur.empty()) tok.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(ch);
            }
        }
    }
    std::string base;
    for (const auto& t : tok) {
        if (t == "l") { r.side = 1; continue; }
        if (t == "r") { r.side = -1; continue; }
        base += t;
    }
    base.erase(std::remove_if(base.begin(), base.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }), base.end());
    auto has = [&](const char* w) { return base.find(w) != std::string::npos; };
    const bool end = has("end") || has("nub") || has("top") || has("tip");
    r.middle_finger = has("middle");
    if (end) return r;
    if (has("finger") || has("thumb") || has("index") || has("middle") || has("ring") || has("pinky") || has("toe")) return r;
    if (has("twist") || has("roll")) return r;  // twist helpers follow their parent
    if (has("hips") || base == "pelvis") r.role = r.side ? Role::Thigh : Role::Hips;
    else if (has("spine") || has("chest") || has("torso")) r.role = Role::Spine;
    else if (has("neck")) r.role = Role::Neck;
    else if (has("head")) r.role = Role::Head;
    else if (has("clavicle") || has("collar") || base == "shoulder") r.role = Role::Clavicle;
    else if (has("forearm") || has("lowerarm") || has("elbow")) r.role = Role::Forearm;
    else if (has("upperarm") || base == "arm" || has("uparm")) r.role = Role::UpperArm;
    else if (base == "hand" || base == "wrist") r.role = Role::Hand;
    else if (has("upleg") || has("thigh") || has("upperleg")) r.role = Role::Thigh;
    else if (base == "leg" || has("calf") || has("shin") || has("lowerleg") || has("knee")) r.role = Role::Shin;
    else if (has("foot") || has("ankle")) r.role = Role::Foot;
    else if (has("tail")) r.role = Role::Tail;
    return r;
}

/// First descendant of j (breadth first) with role r.
int descendant(const Rig& rig, int j, Role r) {
    std::vector<int> q{j};
    for (std::size_t h = 0; h < q.size(); ++h) {
        for (int c = 0; c < int(rig.parent.size()); ++c)
            if (rig.parent[std::size_t(c)] == q[h]) {
                if (rig.role[std::size_t(c)] == r) return c;
                q.push_back(c);
            }
    }
    return -1;
}

void compute_tips(Rig& rig, const std::vector<char>& middle) {
    const std::size_t n = rig.names.size();
    rig.tip.assign(n, {});
    for (std::size_t j = 0; j < n; ++j) {
        const Role want = rig.role[j] == Role::UpperArm ? Role::Forearm
                          : rig.role[j] == Role::Forearm ? Role::Hand
                          : rig.role[j] == Role::Thigh   ? Role::Shin
                          : rig.role[j] == Role::Shin    ? Role::Foot
                                                         : Role::None;
        int c = want != Role::None ? descendant(rig, int(j), want) : -1;
        if (c >= 0) {
            rig.tip[j] = rig.pivot[std::size_t(c)];
            continue;
        }
        // middle finger base for hands, else the mean of the children
        Vec3 sum{0, 0, 0};
        int cnt = 0;
        for (std::size_t k = 0; k < n; ++k)
            if (rig.parent[k] == int(j)) {
                if (rig.role[j] == Role::Hand && !middle.empty() && middle[k]) {
                    sum = rig.pivot[k], cnt = 1;
                    break;
                }
                sum += rig.pivot[k], ++cnt;
            }
        if (cnt > 0 && norm(sum / double(cnt) - rig.pivot[j]) > 0) {
            rig.tip[j] = sum / double(cnt);
        } else {
            const int p = rig.parent[j];
            // leaf: continue the parent bone by half its length
            rig.tip[j] = p >= 0 ? rig.pivot[j] + (rig.pivot[j] - rig.pivot[std::size_t(p)]) * 0.5 : rig.pivot[j] + Vec3{0, 0, 1};
        }
    }
}

}  // namespace

int Rig::find(Role r, int s) const {
    for (std::size_t j = 0; j < role.size(); ++j)
        if (role[j] == r && (s == 0 || side[j] == s)) return int(j);
    return -1;
}

void Rig::scale(double s) {
    for (auto& p : pivot) p = p * s;
    for (auto& p : tip) p = p * s;
}

Rig from_skin(const Skin& skin, std::size_t vertex_count) {
    const std::size_t n = skin.joint_names.size();
    // parents first: order by depth (stable)
    std::vector<int> depth(n, 0);
    for (std::size_t j = 0; j < n; ++j)
        for (int p = skin.parent[j]; p >= 0; p = skin.parent[std::size_t(p)]) ++depth[j];
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return depth[a] < depth[b]; });
    std::vector<int> newi(n);
    for (std::size_t k = 0; k < n; ++k) newi[order[k]] = int(k);
    Rig rig;
    std::vector<char> middle(n);
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t j = order[k];
        rig.names.push_back(skin.joint_names[j]);
        rig.parent.push_back(skin.parent[j] < 0 ? -1 : newi[std::size_t(skin.parent[j])]);
        rig.pivot.push_back(skin.rest_world[j].translation());
        const NameInfo ni = classify(skin.joint_names[j]);
        rig.role.push_back(ni.role);
        int side = ni.side;
        const bool lateral = ni.role == Role::Clavicle || ni.role == Role::UpperArm || ni.role == Role::Forearm ||
                             ni.role == Role::Hand || ni.role == Role::Thigh || ni.role == Role::Shin || ni.role == Role::Foot;
        if (side == 0 && lateral) side = rig.pivot.back().x >= 0 ? 1 : -1;
        rig.side.push_back(lateral ? side : 0);
        middle[k] = ni.middle_finger;
    }
    // a role whose side was named but sits mirrored (rare exporter bug) keeps the name's side
    compute_tips(rig, middle);
    if (skin.joints.size() != vertex_count) throw std::runtime_error("rig: skin data does not match the mesh");
    rig.joints = skin.joints;
    for (auto& jj : rig.joints)
        for (auto& j : jj) j = std::uint16_t(newi[j]);
    rig.weights = skin.weights;
    return rig;
}

// ------------------------------------------------------------------ automatic rig
Rig auto_rig(const Mesh& m) {
    const Box3 b = m.bounds();
    const double H = b.hi.z - b.lo.z;
    if (!(H > 0)) throw std::runtime_error("auto rig: the model has no height");
    const double z0 = b.lo.z;
    // Proportions (fractions of the height) of an 8-head human with the
    // heroic mini canon in mind (larger head, so the joints sit a little
    // lower than in figure.cpp's 1800 mm reference): pelvis 0.52, chest
    // 0.72, neck 0.83, shoulders 0.80, knee 0.27, ankle 0.05.
    auto slab_centroid = [&](double zc, double halfw, const std::function<bool(const Vec3&)>& sel, Vec3 fallback) {
        Vec3 s{0, 0, 0};
        double c = 0;
        for (const auto& v : m.V)
            if (std::abs(v.z - zc) < halfw && sel(v)) s += v, c += 1;
        return c > 0 ? s / c : fallback;
    };
    auto centre_at = [&](double f) {
        const double zc = z0 + f * H;
        Vec3 c = slab_centroid(zc, 0.03 * H, [&](const Vec3& v) { return std::abs(v.x) < 0.1 * H; }, {0, 0, zc});
        return Vec3{0, c.y, zc};
    };
    Rig rig;
    rig.automatic = true;
    auto add = [&](const char* name, int parent, Vec3 p, Role r, int side) {
        rig.names.push_back(name);
        rig.parent.push_back(parent);
        rig.pivot.push_back(p);
        rig.role.push_back(r);
        rig.side.push_back(side);
        return int(rig.names.size()) - 1;
    };
    const int hips = add("hips", -1, centre_at(0.52), Role::Hips, 0);
    const int spine = add("spine", hips, centre_at(0.62), Role::Spine, 0);
    const int chest = add("chest", spine, centre_at(0.72), Role::Spine, 0);
    const int neck = add("neck", chest, centre_at(0.83), Role::Neck, 0);
    const int head = add("head", neck, centre_at(0.88), Role::Head, 0);
    std::vector<Vec3> tips(32);
    for (int side : {1, -1}) {
        const char* L = side > 0 ? "l" : "r";
        const Vec3 sh{side * 0.11 * H, rig.pivot[std::size_t(chest)].y, z0 + 0.80 * H};
        // the hand: the arm-side vertex farthest from the shoulder (T-, A- and arms-down poses)
        Vec3 far = sh + Vec3{side * 0.3 * H, 0, -0.2 * H};
        double best = -1;
        for (const auto& v : m.V)
            if (side * v.x > 0.13 * H && v.z > z0 + 0.3 * H) {
                const double d = norm(v - sh);
                if (d > best) best = d, far = v;
            }
        const int ua = add((std::string("upper_arm_") + L).c_str(), chest, sh, Role::UpperArm, side);
        const int fa = add((std::string("forearm_") + L).c_str(), ua, sh + (far - sh) * 0.46, Role::Forearm, side);
        const int hd = add((std::string("hand_") + L).c_str(), fa, sh + (far - sh) * 0.80, Role::Hand, side);
        tips[std::size_t(hd)] = far;
        auto leg_at = [&](double f) {
            const double zc = z0 + f * H;
            Vec3 c = slab_centroid(zc, 0.02 * H, [&](const Vec3& v) { return side * v.x > 0 && std::abs(v.x) < 0.2 * H; },
                                   {side * 0.055 * H, 0, zc});
            return Vec3{c.x, c.y, zc};
        };
        const Vec3 hip_p{side * 0.055 * H, rig.pivot[std::size_t(hips)].y, z0 + 0.49 * H};
        const int th = add((std::string("thigh_") + L).c_str(), hips, hip_p, Role::Thigh, side);
        const int sn = add((std::string("shin_") + L).c_str(), th, leg_at(0.27), Role::Shin, side);
        const Vec3 ankle = leg_at(0.05);
        const int ft = add((std::string("foot_") + L).c_str(), sn, ankle, Role::Foot, side);
        Vec3 toe = ankle + Vec3{0, -0.08 * H, -0.04 * H};
        double ty = INFINITY;
        for (const auto& v : m.V)
            if (side * v.x > 0 && v.z < z0 + 0.06 * H && v.y < ty) ty = v.y, toe = {ankle.x, v.y, z0 + 0.02 * H};
        tips[std::size_t(ft)] = toe;
    }
    compute_tips(rig, {});
    rig.tip[std::size_t(head)] = {0, rig.pivot[std::size_t(head)].y, b.hi.z};
    for (std::size_t j = 0; j < rig.names.size(); ++j)
        if (rig.role[j] == Role::Hand || rig.role[j] == Role::Foot) rig.tip[j] = tips[j];

    geodesic_weights(rig, m);
    return rig;
}

// ------------------------------------------------------------------ posing
std::vector<Xf> pose_joints(const Rig& rig, const std::map<std::string, double>& pose) {
    auto ang = [&](const std::string& k) {
        auto it = pose.find(k);
        return it == pose.end() ? 0.0 : it->second;
    };
    const std::size_t n = rig.names.size();
    std::size_t n_spine = 0, n_tail = 0;
    for (auto r : rig.role) n_spine += r == Role::Spine, n_tail += r == Role::Tail;
    std::vector<Quat> F(n), G(n);
    std::vector<Xf> X(n);
    for (std::size_t j = 0; j < n; ++j) {
        const int p = rig.parent[j];
        const Quat Fp = p < 0 ? Quat{} : F[std::size_t(p)], Gp = p < 0 ? Quat{} : G[std::size_t(p)];
        const int s = rig.side[j];
        const std::string pre = s > 0 ? "l_" : "r_";
        auto local = [&](const Quat& L) { F[j] = Fp * L, G[j] = Gp * L; };
        switch (rig.role[j]) {
            case Role::Spine: local(rx(ang("spine") / double(std::max<std::size_t>(n_spine, 1)))); break;
            case Role::Tail: local(rx(ang("tail_curl") / double(std::max<std::size_t>(n_tail, 1)))); break;
            case Role::Neck: local(rx(ang("neck"))); break;
            case Role::Head: local(rz(ang("head_turn")) * rx(ang("head_tilt"))); break;
            case Role::UpperArm: {
                // as figure.cpp: abduction about Y (outwards), then flexion about X (forwards)
                F[j] = Fp * ry(-s * ((rig.normalize_arms ? kArmOut : 0.0) + ang(pre + "shoulder_raise"))) * rx(-ang(pre + "shoulder_fwd"));
                G[j] = rig.normalize_arms ? F[j] * Quat::from_to(rig.tip[j] - rig.pivot[j], kDown) : F[j];
                break;
            }
            case Role::Forearm: {
                F[j] = Fp * rx(-ang(pre + "elbow"));
                G[j] = rig.normalize_arms ? F[j] * Quat::from_to(rig.tip[j] - rig.pivot[j], kDown) : F[j];
                break;
            }
            case Role::Thigh: local(rx(-ang(pre + "hip_fwd")) * ry(-s * ang(pre + "hip_side"))); break;
            case Role::Shin: local(rx(ang(pre + "knee"))); break;
            case Role::Foot: F[j] = G[j] = Quat{}; break;  // feet stay level on the ground (as figure.cpp)
            default: F[j] = Fp, G[j] = Gp; break;          // hips, clavicles, hands, fingers, helpers follow
        }
        const Vec3 P = p < 0 ? rig.pivot[j] : X[std::size_t(p)].apply(rig.pivot[j]);
        X[j].q = G[j];
        X[j].t = P - G[j].rotate(rig.pivot[j]);
    }
    return X;
}

Mesh deform(const Mesh& rest, const Rig& rig, const std::vector<Xf>& X) {
    Mesh out = rest;
    if (rig.joints.size() != rest.V.size()) return out;
    // dual parts q_d = 1/2 (0, t) q_r of every joint
    std::vector<Quat> qd(X.size());
    for (std::size_t j = 0; j < X.size(); ++j) {
        const Quat t{0, X[j].t.x, X[j].t.y, X[j].t.z};
        const Quat d = t * X[j].q;
        qd[j] = {0.5 * d.w, 0.5 * d.x, 0.5 * d.y, 0.5 * d.z};
    }
    parallel_for(rest.V.size(), [&](std::size_t i) {
        const auto& jj = rig.joints[i];
        const auto& ww = rig.weights[i];
        Quat b0{0, 0, 0, 0}, be{0, 0, 0, 0};
        const Quat& pivot = X[jj[0]].q;
        double wsum = 0;
        for (int k = 0; k < 4; ++k) {
            const double w = ww[std::size_t(k)];
            if (w <= 0) continue;
            const Quat& q = X[jj[std::size_t(k)]].q;
            const Quat& d = qd[jj[std::size_t(k)]];
            // antipodality: q and -q are the same rotation; blend on one hemisphere
            const double sgn = (q.w * pivot.w + q.x * pivot.x + q.y * pivot.y + q.z * pivot.z) < 0 ? -w : w;
            b0 = {b0.w + sgn * q.w, b0.x + sgn * q.x, b0.y + sgn * q.y, b0.z + sgn * q.z};
            be = {be.w + sgn * d.w, be.x + sgn * d.x, be.y + sgn * d.y, be.z + sgn * d.z};
            wsum += w;
        }
        if (wsum <= 0) return;  // static vertex
        const double nrm = std::sqrt(b0.w * b0.w + b0.x * b0.x + b0.y * b0.y + b0.z * b0.z);
        b0 = {b0.w / nrm, b0.x / nrm, b0.y / nrm, b0.z / nrm};
        be = {be.w / nrm, be.x / nrm, be.y / nrm, be.z / nrm};
        // translation t = 2 q_e q_r*
        const Quat t = be * b0.conj();
        out.V[i] = b0.rotate(rest.V[i]) + Vec3{2 * t.x, 2 * t.y, 2 * t.z};
    });
    return out;
}

Xf hand_grip(const Rig& rig, const std::vector<Xf>& X, int side) {
    const int hand = rig.find(Role::Hand, side);
    const int fore = rig.find(Role::Forearm, side);
    if (hand < 0 || fore < 0)
        throw std::runtime_error(std::string("the model has no ") + (side > 0 ? "left" : "right") + " hand in its rig");
    const auto h = std::size_t(hand), f = std::size_t(fore);
    // canonical forearm frame F = G A^-1 (pose_joints: G = F A)
    const Quat A = rig.normalize_arms ? Quat::from_to(rig.tip[f] - rig.pivot[f], kDown) : Quat{};
    Xf g;
    g.q = X[f].q * A.conj();
    // fist centre: 70 % of the way from the wrist to the knuckles (the
    // fingers wrap around the grip just below the knuckle line)
    g.t = X[h].apply(rig.pivot[h] + (rig.tip[h] - rig.pivot[h]) * 0.7);
    return g;
}

namespace {
double seg_dist(const Vec3& p, const Vec3& a, const Vec3& b) {
    const Vec3 ab = b - a;
    const double l2 = dot(ab, ab);
    const double t = l2 > 0 ? std::clamp(dot(p - a, ab) / l2, 0.0, 1.0) : 0.0;
    return norm(p - (a + ab * t));
}
// vertex one-rings (unique neighbours)
std::vector<std::vector<std::uint32_t>> one_rings(const Mesh& m) {
    std::vector<std::vector<std::uint32_t>> r(m.V.size());
    for (const auto& f : m.F)
        for (int k = 0; k < 3; ++k) r[f[std::size_t(k)]].push_back(f[std::size_t((k + 1) % 3)]), r[f[std::size_t(k)]].push_back(f[std::size_t((k + 2) % 3)]);
    for (auto& v : r) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }
    return r;
}
// keep the 4 largest weights, normalised
void store_top4(Rig& rig, std::size_t i, std::vector<std::pair<double, int>>& c) {
    std::sort(c.begin(), c.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (c.size() > 4) c.resize(4);
    double sum = 0;
    for (const auto& x : c) sum += x.first;
    rig.joints[i] = {0, 0, 0, 0};
    rig.weights[i] = {0, 0, 0, 0};
    for (std::size_t k = 0; k < c.size(); ++k) {
        rig.joints[i][k] = std::uint16_t(c[k].second);
        rig.weights[i][k] = float(sum > 0 ? c[k].first / sum : (k == 0 ? 1.0 : 0.0));
    }
}
}  // namespace

void geodesic_weights(Rig& rig, const Mesh& m) {
    const Box3 bb = m.bounds();
    const double H = std::max({bb.hi.z - bb.lo.z, bb.hi.x - bb.lo.x, bb.hi.y - bb.lo.y});
    if (!(H > 0) || rig.names.empty()) return;
    // ~160 voxels over the largest extent: limbs of a 32 mm mini get >= 4 voxels across
    const double h = sdf::voxel_step(H / 160);
    const sdf::Grid g = sdf::from_mesh_robust(m, h, nullptr, false);
    const int i0 = int(std::floor(bb.lo.x / h)) - 2, j0 = int(std::floor(bb.lo.y / h)) - 2, k0 = int(std::floor(bb.lo.z / h)) - 2;
    const int nx = int(std::ceil(bb.hi.x / h)) + 3 - i0, ny = int(std::ceil(bb.hi.y / h)) + 3 - j0, nz = int(std::ceil(bb.hi.z / h)) + 3 - k0;
    auto lin = [&](int i, int j, int k) { return (std::size_t(k) * std::size_t(ny) + std::size_t(j)) * std::size_t(nx) + std::size_t(i); };
    std::vector<std::int32_t> id(std::size_t(nx) * std::size_t(ny) * std::size_t(nz), -1);
    std::vector<std::array<int, 3>> cell;
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                if (g.at(i0 + i, j0 + j, k0 + k) < 0) id[lin(i, j, k)] = std::int32_t(cell.size()), cell.push_back({i, j, k});
    if (cell.empty()) return;
    auto pos = [&](std::size_t c) { return Vec3{(i0 + cell[c][0]) * h, (j0 + cell[c][1]) * h, (k0 + cell[c][2]) * h}; };
    const std::size_t nb = rig.names.size(), nc = cell.size();
    const double dmax = 0.5 * H;
    std::vector<std::vector<float>> geo(nb, std::vector<float>(nc, INFINITY));
    parallel_for(nb, [&](std::size_t b) {
        auto& d = geo[b];
        using QE = std::pair<double, std::int32_t>;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
        // seeds: inside voxels along the bone (else the inside voxel nearest to it)
        const Vec3 a = rig.pivot[b], t = rig.tip[b];
        const int steps = std::max(1, int(norm(t - a) / (0.5 * h)));
        for (int s = 0; s <= steps; ++s) {
            const Vec3 p = a + (t - a) * (double(s) / steps);
            const int i = int(std::lround(p.x / h)) - i0, j = int(std::lround(p.y / h)) - j0, k = int(std::lround(p.z / h)) - k0;
            if (i < 0 || j < 0 || k < 0 || i >= nx || j >= ny || k >= nz) continue;
            const std::int32_t c = id[lin(i, j, k)];
            if (c < 0) continue;
            const double d0 = norm(pos(std::size_t(c)) - p);
            if (d0 < d[std::size_t(c)]) d[std::size_t(c)] = float(d0), q.push({d0, c});
        }
        if (q.empty()) {
            std::size_t best = 0;
            double bd = INFINITY;
            for (std::size_t c = 0; c < nc; ++c)
                if (const double dd = seg_dist(pos(c), a, t); dd < bd) bd = dd, best = c;
            d[best] = float(bd);
            q.push({bd, std::int32_t(best)});
        }
        while (!q.empty()) {
            const auto [dc, c] = q.top();
            q.pop();
            if (dc > d[std::size_t(c)] || dc > dmax) continue;
            const auto& cc = cell[std::size_t(c)];
            for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int n2 = dx * dx + dy * dy + dz * dz;
                        if (!n2) continue;
                        const int i = cc[0] + dx, j = cc[1] + dy, k = cc[2] + dz;
                        if (i < 0 || j < 0 || k < 0 || i >= nx || j >= ny || k >= nz) continue;
                        const std::int32_t e = id[lin(i, j, k)];
                        if (e < 0) continue;
                        const double nd = dc + h * std::sqrt(double(n2));
                        if (nd < d[std::size_t(e)]) d[std::size_t(e)] = float(nd), q.push({nd, e});
                    }
        }
    });
    const double sigma = 0.02 * H;
    rig.joints.assign(m.V.size(), {0, 0, 0, 0});
    rig.weights.assign(m.V.size(), {0, 0, 0, 0});
    parallel_for(m.V.size(), [&](std::size_t vi) {
        const Vec3& p = m.V[vi];
        // geodesic distance of the vertex: through the inside voxels around it
        std::vector<double> dv(nb, INFINITY);
        const int ci = int(std::lround(p.x / h)) - i0, cj = int(std::lround(p.y / h)) - j0, ck = int(std::lround(p.z / h)) - k0;
        for (int r = 1; r <= 3; ++r) {
            bool found = false;
            for (int dz = -r; dz <= r; ++dz)
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx) {
                        const int i = ci + dx, j = cj + dy, k = ck + dz;
                        if (i < 0 || j < 0 || k < 0 || i >= nx || j >= ny || k >= nz) continue;
                        const std::int32_t c = id[lin(i, j, k)];
                        if (c < 0) continue;
                        found = true;
                        const double off = norm(pos(std::size_t(c)) - p);
                        for (std::size_t b = 0; b < nb; ++b) dv[b] = std::min(dv[b], double(geo[b][std::size_t(c)]) + off);
                    }
            if (found) break;
        }
        double dmin = *std::min_element(dv.begin(), dv.end());
        std::vector<std::pair<double, int>> c;
        if (std::isfinite(dmin)) {
            for (std::size_t b = 0; b < nb; ++b)
                if (dv[b] <= dmin + 3 * sigma) c.push_back({std::exp(-(dv[b] - dmin) / sigma), int(b)});
        } else {  // isolated sliver outside the voxel solid: nearest bone in space
            double best = INFINITY;
            int bi = 0;
            for (std::size_t b = 0; b < nb; ++b)
                if (const double dd = seg_dist(p, rig.pivot[b], rig.tip[b]); dd < best) best = dd, bi = int(b);
            c.push_back({1.0, bi});
        }
        store_top4(rig, vi, c);
    });
    // two smoothing passes over the one-rings (soft joints without seams)
    const auto ring = one_rings(m);
    for (int pass = 0; pass < 2; ++pass) {
        const auto J = rig.joints;
        const auto W = rig.weights;
        parallel_for(m.V.size(), [&](std::size_t vi) {
            std::unordered_map<int, double> acc;
            for (int k = 0; k < 4; ++k) acc[J[vi][std::size_t(k)]] += 0.5 * W[vi][std::size_t(k)];
            const double wn = ring[vi].empty() ? 0.0 : 0.5 / double(ring[vi].size());
            for (auto u : ring[vi])
                for (int k = 0; k < 4; ++k) acc[J[u][std::size_t(k)]] += wn * W[u][std::size_t(k)];
            std::vector<std::pair<double, int>> c;
            for (const auto& [b, w] : acc)
                if (w > 0) c.push_back({w, b});
            if (ring[vi].empty())
                for (auto& x : c) x.first *= 2;
            store_top4(rig, vi, c);
        });
    }
}

std::size_t rigidify_parts(Rig& rig, const Mesh& m, double max_size) {
    if (rig.joints.size() != m.V.size() || m.V.empty()) return 0;
    // connected components over shared vertices (union-find)
    std::vector<std::uint32_t> uf(m.V.size());
    std::iota(uf.begin(), uf.end(), 0u);
    std::function<std::uint32_t(std::uint32_t)> find = [&](std::uint32_t x) {
        while (uf[x] != x) x = uf[x] = uf[uf[x]];
        return x;
    };
    for (const auto& f : m.F)
        for (int k = 1; k < 3; ++k) {
            const auto a = find(f[0]), b = find(f[std::size_t(k)]);
            if (a != b) uf[std::max(a, b)] = std::min(a, b);
        }
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> comp;
    for (std::uint32_t v = 0; v < m.V.size(); ++v) comp[find(v)].push_back(v);
    if (comp.size() < 2) return 0;  // one connected mesh: nothing to separate
    std::size_t n = 0;
    for (const auto& [r, vs] : comp) {  // the body itself is larger than max_size
        Box3 b;
        Vec3 c{0, 0, 0};
        std::vector<char> used(rig.names.size(), 0);
        for (auto v : vs) {
            b.add(m.V[v]), c += m.V[v];
            for (int k = 0; k < 4; ++k)
                if (rig.weights[v][std::size_t(k)] > 0.05f) used[rig.joints[v][std::size_t(k)]] = 1;
        }
        if (b.diagonal() > max_size) continue;
        c = c / double(vs.size());
        int best = -1;
        double bd = INFINITY;
        for (int pass = 0; pass < 2 && best < 0; ++pass)  // bones carrying weight first, then any
            for (std::size_t j = 0; j < rig.names.size(); ++j) {
                if (pass == 0 && !used[j]) continue;
                if (const double d = seg_dist(c, rig.pivot[j], rig.tip[j]); d < bd) bd = d, best = int(j);
            }
        if (best < 0) continue;
        for (auto v : vs) {
            rig.joints[v] = {std::uint16_t(best), 0, 0, 0};
            rig.weights[v] = {1, 0, 0, 0};
        }
        ++n;
    }
    rig.rigid_parts = n;
    return n;
}

Distortion distortion(const Mesh& rest, const Mesh& posed) {
    Distortion d;
    if (rest.V.size() != posed.V.size() || rest.F.empty()) return d;
    std::vector<double> r;
    r.reserve(rest.F.size() * 3);
    for (const auto& f : rest.F)
        for (int k = 0; k < 3; ++k) {
            const auto a = f[std::size_t(k)], b = f[std::size_t((k + 1) % 3)];
            if (a > b) continue;  // each interior edge once (both orientations share it)
            const double l0 = norm(rest.V[b] - rest.V[a]), l1 = norm(posed.V[b] - posed.V[a]);
            if (l0 <= 0 || l1 <= 0) continue;
            r.push_back(std::max(l1 / l0, l0 / l1));
        }
    if (r.empty()) return d;
    std::sort(r.begin(), r.end());
    d.max_ratio = r.back();
    d.p99_ratio = r[std::min(r.size() - 1, std::size_t(0.99 * double(r.size())))];
    d.frac_over_1_5 = double(r.end() - std::upper_bound(r.begin(), r.end(), 1.5)) / double(r.size());
    return d;
}

std::shared_ptr<const ModelAsset> make_asset(ModelData md, double height0, const AssetOptions& opt) {
    fit_to_height(md, height0);
    auto a = std::make_shared<ModelAsset>();
    const bool humanoid_skin = md.skin && [&] {
        Rig r = from_skin(*md.skin, md.mesh.V.size());
        return r.find(Role::UpperArm, 1) >= 0 && r.find(Role::Thigh, 1) >= 0;
    }();
    if (humanoid_skin) {
        a->rig = from_skin(*md.skin, md.mesh.V.size());
        if (opt.weights == "auto") geodesic_weights(a->rig, md.mesh);  // the file's skeleton, our weights
    } else {
        // no skin, or one without recognisable humanoid bones (props, creatures): fit our own
        a->rig = auto_rig(md.mesh);
    }
    if (opt.rigid) rigidify_parts(a->rig, md.mesh, kRigidPartFraction * height0);
    a->mesh = std::move(md.mesh);
    a->height0 = height0;
    a->format = md.format;
    a->parts = md.parts;
    return a;
}

}  // namespace anvil::rig
