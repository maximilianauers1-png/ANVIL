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

    // ---- weights. Bone j = segment pivot_j -> tip_j. Influences: bones within
    // 3 sigma of the nearest one whose straight path to the vertex runs inside
    // the solid (winding number of the capped mesh at the midpoint), weighted
    // exp(-(d - d_min) / sigma). sigma = 1.2 % of the height (0.4 mm on a 32 mm
    // mini): the blend zone around a joint spans ~ +-3 sigma ~ the limb radius.
    const double sigma = 0.012 * H;
    Mesh capped = m;
    cap_boundaries(capped);
    const WindingNumber wn(capped, tol::kWindingBeta);
    const std::size_t nb = rig.names.size();
    rig.joints.assign(m.V.size(), {0, 0, 0, 0});
    rig.weights.assign(m.V.size(), {0, 0, 0, 0});
    parallel_for(m.V.size(), [&](std::size_t i) {
        const Vec3& v = m.V[i];
        std::vector<std::pair<double, int>> d;
        std::vector<Vec3> foot(nb);
        for (std::size_t j = 0; j < nb; ++j) {
            const Vec3 a = rig.pivot[j], ab = rig.tip[j] - a;
            const double l2 = dot(ab, ab);
            const double t = l2 > 0 ? std::clamp(dot(v - a, ab) / l2, 0.0, 1.0) : 0.0;
            foot[j] = a + ab * t;
            d.push_back({norm(v - foot[j]), int(j)});
        }
        std::sort(d.begin(), d.end());
        std::vector<std::pair<double, int>> cand, vis;
        for (const auto& c : d)
            if (c.first <= d[0].first + 3 * sigma && cand.size() < 4) cand.push_back(c);
        if (cand.size() == 1) vis = cand;  // a single candidate needs no visibility test
        else
            for (const auto& [dj, j] : cand)
                if (std::abs(wn((v + foot[std::size_t(j)]) * 0.5)) > 0.5) vis.push_back({dj, j});
        if (vis.empty()) vis.push_back(d[0]);
        double sum = 0;
        std::array<double, 4> w{};
        for (std::size_t k = 0; k < vis.size(); ++k) sum += (w[k] = std::exp(-(vis[k].first - vis[0].first) / sigma));
        for (std::size_t k = 0; k < vis.size(); ++k) {
            rig.joints[i][k] = std::uint16_t(vis[k].second);
            rig.weights[i][k] = float(w[k] / sum);
        }
    });
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
                F[j] = Fp * ry(-s * (kArmOut + ang(pre + "shoulder_raise"))) * rx(-ang(pre + "shoulder_fwd"));
                G[j] = F[j] * Quat::from_to(rig.tip[j] - rig.pivot[j], kDown);
                break;
            }
            case Role::Forearm: {
                F[j] = Fp * rx(-ang(pre + "elbow"));
                G[j] = F[j] * Quat::from_to(rig.tip[j] - rig.pivot[j], kDown);
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
    const Quat A = Quat::from_to(rig.tip[f] - rig.pivot[f], kDown);
    Xf g;
    g.q = X[f].q * A.conj();
    // fist centre: 70 % of the way from the wrist to the knuckles (the
    // fingers wrap around the grip just below the knuckle line)
    g.t = X[h].apply(rig.pivot[h] + (rig.tip[h] - rig.pivot[h]) * 0.7);
    return g;
}

std::shared_ptr<const ModelAsset> make_asset(ModelData md, double height0) {
    fit_to_height(md, height0);
    auto a = std::make_shared<ModelAsset>();
    a->rig = md.skin ? from_skin(*md.skin, md.mesh.V.size()) : auto_rig(md.mesh);
    if (md.skin && (a->rig.find(Role::UpperArm, 1) < 0 || a->rig.find(Role::Thigh, 1) < 0)) {
        // a skin without recognisable humanoid bones (props, creatures): fit our own
        a->rig = auto_rig(md.mesh);
    }
    a->mesh = std::move(md.mesh);
    a->height0 = height0;
    a->format = md.format;
    a->parts = md.parts;
    return a;
}

}  // namespace anvil::rig
