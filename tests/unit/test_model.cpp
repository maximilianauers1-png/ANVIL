// M4: model import (glTF/OBJ), winding numbers, robust voxelization, rig + DQS.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <cstring>
#include <numbers>
#include <random>

#include "anvil/figure.hpp"
#include "anvil/model_io.hpp"
#include "anvil/rig.hpp"
#include "anvil/sdf.hpp"
#include "anvil/validate.hpp"
#include "anvil/winding.hpp"

using namespace anvil;

namespace {
Mesh uv_sphere(double r, int nu = 48, int nv = 24, Vec3 c = {}) {
    Mesh m;
    m.V.push_back(c + Vec3{0, 0, r});
    for (int j = 1; j < nv; ++j)
        for (int i = 0; i < nu; ++i) {
            const double th = std::numbers::pi * j / nv, ph = 2 * std::numbers::pi * i / nu;
            m.V.push_back(c + Vec3{r * std::sin(th) * std::cos(ph), r * std::sin(th) * std::sin(ph), r * std::cos(th)});
        }
    m.V.push_back(c + Vec3{0, 0, -r});
    auto id = [&](int j, int i) { return std::uint32_t(1 + (j - 1) * nu + (i % nu)); };
    for (int i = 0; i < nu; ++i) m.F.push_back({0, id(1, i), id(1, i + 1)});
    for (int j = 1; j < nv - 1; ++j)
        for (int i = 0; i < nu; ++i) {
            m.F.push_back({id(j, i), id(j + 1, i), id(j + 1, i + 1)});
            m.F.push_back({id(j, i), id(j + 1, i + 1), id(j, i + 1)});
        }
    const auto s = std::uint32_t(m.V.size() - 1);
    for (int i = 0; i < nu; ++i) m.F.push_back({s, id(nv - 1, i + 1), id(nv - 1, i)});
    return m;
}
Mesh box(Vec3 lo, Vec3 hi, bool open_top = false) {
    Mesh m;
    for (int k = 0; k < 8; ++k) m.V.push_back({k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z});
    const std::uint32_t f[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
                                    {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
    for (int t = 0; t < 12; ++t)
        if (!(open_top && (t == 2 || t == 3))) m.F.push_back({f[t][0], f[t][1], f[t][2]});
    return m;
}
std::string b64(const std::string& in) {
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c, bits += 8;
        while (bits >= 0) out.push_back(a[(val >> bits) & 0x3F]), bits -= 6;
    }
    if (bits > -6) out.push_back(a[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}
template <class T>
void put(std::string& s, T v) {
    s.append(reinterpret_cast<const char*>(&v), sizeof v);
}
}  // namespace

TEST_CASE("winding number: 1 inside, 0 outside; fast = exact within the dipole error", "[model]") {
    const Mesh s = uv_sphere(10);
    WindingNumber wn(s, 2.0);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(-20, 20);
    double worst = 0;
    for (int i = 0; i < 400; ++i) {
        const Vec3 p{u(rng), u(rng), u(rng)};
        const double r = norm(p);
        if (std::abs(r - 10) < 0.5) continue;
        const double w = wn(p), we = wn.exact(p);
        worst = std::max(worst, std::abs(w - we));
        CHECK(std::abs(we - (r < 10 ? 1.0 : 0.0)) < 1e-9);
    }
    CHECK(worst < 0.05);  // far below the 1/2 threshold
}

TEST_CASE("cap_boundaries closes holes so the winding number is integer again", "[model]") {
    Mesh m = box({0, 0, 0}, {10, 10, 10}, true);
    WindingNumber open(m, 2.0);
    CHECK(open.exact({5, 5, 9}) < 0.9);  // near the hole the open box reads fractional
    const std::size_t added = cap_boundaries(m);
    CHECK(added == 4);
    WindingNumber closed(m, 2.0);
    CHECK(closed.exact({5, 5, 9}) == Catch::Approx(1.0).margin(1e-9));
    CHECK(closed.exact({5, 5, 11}) == Catch::Approx(0.0).margin(1e-9));
}

TEST_CASE("robust voxelization: open, overlapping and flipped shells give the union", "[model]") {
    Mesh m = box({0, 0, 0}, {10, 10, 10}, true);  // open top
    Mesh b2 = box({5, 0, 0}, {15, 10, 10});      // overlaps the first
    m.append(b2);
    Mesh flipped = box({0, 20, 0}, {4, 24, 4});
    for (auto& f : flipped.F) std::swap(f[1], f[2]);  // inside-out third shell
    m.append(flipped);
    std::size_t capped = 0;
    const auto g = sdf::from_mesh_robust(m, 0.25, &capped);
    CHECK(capped == 4);
    const Mesh out = sdf::dual_contour(g);
    ValidationOptions vo;
    vo.self_intersections = false;
    CHECK(validate(out, vo).ok);
    // union volume 15*10*10 + 4^3; the contoured box loses at most the corner
    // bevel (~h per edge length) -- 2 %
    CHECK(out.signed_volume() == Catch::Approx(1500.0 + 64.0).epsilon(0.02));
}

TEST_CASE("glTF: frame change, node hierarchy, skin at rest, GLB container", "[model]") {
    // one triangle strip of 3 vertices skinned to two joints; joint 1 rotated
    // 90 degrees about Z at the bind pose
    std::string bin;
    const float pos[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    for (float f : pos) put(bin, f);
    const std::uint16_t idx[] = {0, 1, 2, 0};
    for (auto i : idx) put(bin, i);
    const std::uint8_t joints[] = {0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0};
    for (auto j : joints) put(bin, j);
    const float weights[] = {1, 0, 0, 0, 1, 0, 0, 0, 0.5f, 0.5f, 0, 0};
    for (float w : weights) put(bin, w);
    const float ibm[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (float f : ibm) put(bin, f);
    const std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,2]}],
      "nodes":[{"name":"mixamorig:Hips","children":[1]},
               {"name":"mixamorig:Spine","translation":[0,0,0],"rotation":[0,0,0.7071067811865476,0.7071067811865476]},
               {"mesh":0,"skin":0,"translation":[5,5,5]}],
      "skins":[{"joints":[0,1],"inverseBindMatrices":4}],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":2,"WEIGHTS_0":3},"indices":1}]}],
      "buffers":[{"byteLength":)" + std::to_string(bin.size()) + R"(}],
      "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6},
                     {"buffer":0,"byteOffset":44,"byteLength":12},{"buffer":0,"byteOffset":56,"byteLength":48},
                     {"buffer":0,"byteOffset":104,"byteLength":128}],
      "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
                   {"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"},
                   {"bufferView":2,"componentType":5121,"count":3,"type":"VEC4"},
                   {"bufferView":3,"componentType":5126,"count":3,"type":"VEC4"},
                   {"bufferView":4,"componentType":5126,"count":2,"type":"MAT4"}]})";
    // the u16 index data is padded to 8 bytes (4th index unused) so the next view stays aligned
    std::string padded = bin;
    std::string glb;
    std::string js = json;
    while (js.size() % 4) js.push_back(' ');
    while (padded.size() % 4) padded.push_back('\0');
    put(glb, std::uint32_t(0x46546C67));
    put(glb, std::uint32_t(2));
    put(glb, std::uint32_t(12 + 8 + js.size() + 8 + padded.size()));
    put(glb, std::uint32_t(js.size()));
    put(glb, std::uint32_t(0x4E4F534A));
    glb += js;
    put(glb, std::uint32_t(padded.size()));
    put(glb, std::uint32_t(0x004E4942));
    glb += padded;
    const ModelData md = decode_glb(glb, ".");
    REQUIRE(md.mesh.V.size() == 3);
    REQUIRE(md.skin);
    // vertex 0: joint 0 (identity) -> (0,0,0); the mesh node's own transform is ignored (spec 3.7.3)
    CHECK(norm(md.mesh.V[0]) < 1e-9);
    // vertex 1: joint 1 = Rz(90) * IBM(identity): (1,0,0) -> (0,1,0) m -> ANVIL (0, 0, 1000) mm
    CHECK(norm(md.mesh.V[1] - Vec3{0, 0, 1000}) < 1e-3);
    // vertex 2: half/half of (0,1,0) and (-1,0,0) -> (-0.5,0.5,0) m -> (-500, 0, 500) mm
    CHECK(norm(md.mesh.V[2] - Vec3{-500, 0, 500}) < 1e-3);
    CHECK(md.skin->joint_names[1] == "mixamorig:Spine");
    CHECK(md.skin->parent[1] == 0);
    // the json-only path with a data URI decodes the same
    std::string json2 = json;
    const std::string key = R"("buffers":[{"byteLength":)";
    const auto at = json2.find(key);
    json2.replace(at, key.size() + std::to_string(bin.size()).size() + 2,
                  R"("buffers":[{"uri":"data:application/octet-stream;base64,)" + b64(padded) + R"(","byteLength":)" +
                      std::to_string(padded.size()) + "}]");
    const ModelData md2 = decode_gltf_json(json2, {}, ".");
    CHECK(norm(md2.mesh.V[2] - md.mesh.V[2]) < 1e-9);
    // Draco is refused with a precise message
    CHECK_THROWS_WITH(decode_gltf_json(R"({"asset":{"version":"2.0"},"extensionsRequired":["KHR_draco_mesh_compression"]})", {}, "."),
                      Catch::Matchers::ContainsSubstring("KHR_draco_mesh_compression"));
}

TEST_CASE("OBJ: polygons, negative indices, Y up -> Z up", "[model]") {
    const ModelData md = decode_obj("o quad\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf -4 -3 -2 -1\n");
    CHECK(md.mesh.F.size() == 2);
    CHECK(md.mesh.V[2] == Vec3{1, 0, 1});  // (x, y, z) -> (x, -z, y)
}

TEST_CASE("rig: bone names, forward kinematics, DQS keeps rigid parts rigid and bends with volume", "[model]") {
    // a straight arm along +X: shoulder at 0, elbow at 10, wrist at 20
    Skin sk;
    sk.joint_names = {"mixamorig:Spine1", "mixamorig:LeftArm", "mixamorig:LeftForeArm", "mixamorig:LeftHand"};
    sk.parent = {-1, 0, 1, 2};
    for (double x : {0.0, 0.0, 10.0, 20.0}) {
        Mat4d m;
        m.m[12] = x;
        sk.rest_world.push_back(m);
    }
    // a tube around the arm, rings every 0.5 mm, weights blended over the elbow
    Mesh tube;
    const int sides = 16;
    for (int k = 0; k <= 40; ++k)
        for (int s = 0; s < sides; ++s) {
            const double x = k * 0.5, a = 2 * std::numbers::pi * s / sides;
            tube.V.push_back({x, 2 * std::cos(a), 2 * std::sin(a)});
            const double t = std::clamp((x - 8) / 4, 0.0, 1.0);
            sk.joints.push_back({1, 2, 0, 0});
            sk.weights.push_back({float(1 - t), float(t), 0, 0});
        }
    for (int k = 0; k < 40; ++k)
        for (int s = 0; s < sides; ++s) {
            const auto i0 = std::uint32_t(k * sides + s), i1 = std::uint32_t(k * sides + (s + 1) % sides);
            tube.F.push_back({i0, i1, i1 + sides});
            tube.F.push_back({i0, i1 + sides, i0 + sides});
        }
    const rig::Rig r = rig::from_skin(sk, tube.V.size());
    CHECK(r.role[1] == rig::Role::UpperArm);
    CHECK(r.role[2] == rig::Role::Forearm);
    CHECK(r.role[3] == rig::Role::Hand);
    CHECK(r.side[1] == 1);
    // zero pose: the horizontal (T-pose) arm hangs down, 10 degrees out
    auto X = rig::pose_joints(r, {});
    const Vec3 wrist = X[3].apply(r.pivot[3]);
    CHECK(wrist.z == Catch::Approx(-20 * std::cos(0.17)).margin(1e-9));
    CHECK(wrist.x == Catch::Approx(20 * std::sin(0.17)).margin(1e-9));
    // elbow 90 degrees: the forearm points forwards (-Y), perpendicular to the upper arm
    X = rig::pose_joints(r, {{"l_elbow", std::numbers::pi / 2}});
    const Vec3 e = X[2].apply(r.pivot[2]), w = X[3].apply(r.pivot[3]);
    CHECK(dot(normalized(w - e), normalized(e - X[1].apply(r.pivot[1]))) == Catch::Approx(0).margin(1e-9));
    CHECK((w - e).y < -9.99);
    const Mesh bent = rig::deform(tube, r, X);
    // rigid parts move rigidly: ring distances to the bone stay 2
    for (std::size_t i = 0; i < tube.V.size(); ++i)
        if (tube.V[i].x < 7.9) {
            const Vec3 s0 = X[1].apply(r.pivot[1]), s1 = e;
            const Vec3 d = normalized(s1 - s0);
            const Vec3 q = bent.V[i] - s0;
            CHECK(norm(q - d * dot(q, d)) == Catch::Approx(2).epsilon(1e-9));
        }
    // DQS keeps the cross-section at the bent elbow: the elbow ring (x = 10)
    // stays at least 1.6 mm from the joint (linear blending collapses it to ~1.41)
    double rmin = 1e9;
    for (std::size_t i = 0; i < tube.V.size(); ++i)
        if (std::abs(tube.V[i].x - 10) < 1e-9) rmin = std::min(rmin, norm(bent.V[i] - e));
    CHECK(rmin > 1.6);
}

TEST_CASE("auto rig fits a T-posed figure and posing moves only the arm", "[model]") {
    figure::Params fp;
    fp.height = 32;
    fp.pose["l_shoulder_raise"] = 1.4;  // ~T-pose
    fp.pose["r_shoulder_raise"] = 1.4;
    const figure::Body body = figure::make_body(fp);
    const auto g = sdf::from_function([&](const Vec3& p) { return body.sdf(p); }, body.bounds, 0.2, body.blend / 4);
    const Mesh m = sdf::dual_contour(g);
    const rig::Rig r = rig::auto_rig(m);
    const int lh = r.find(rig::Role::Hand, 1), rf = r.find(rig::Role::Foot, -1);
    REQUIRE(lh >= 0);
    REQUIRE(rf >= 0);
    CHECK(r.pivot[std::size_t(lh)].x > 8);     // the left hand is far out on +X
    CHECK(r.pivot[std::size_t(rf)].z < 3);     // feet near the ground
    // raising the right arm forwards leaves the left foot where it was
    const auto X = rig::pose_joints(r, {{"r_shoulder_fwd", 1.5}});
    const Mesh posed = rig::deform(m, r, X);
    double foot_move = 0, hand_move = 0;
    for (std::size_t i = 0; i < m.V.size(); ++i) {
        const double d = norm(posed.V[i] - m.V[i]);
        if (m.V[i].z < 3 && m.V[i].x > 0) foot_move = std::max(foot_move, d);
        if (m.V[i].x < -9) hand_move = std::max(hand_move, d);
    }
    CHECK(foot_move < 1e-9);
    CHECK(hand_move > 5);
}
