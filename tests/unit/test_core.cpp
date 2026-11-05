// Unit and property tests for anvil_core.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_random.hpp>

#include <cmath>
#include <filesystem>
#include <numbers>
#include <random>

#include "anvil/bvh.hpp"
#include "anvil/camera.hpp"
#include "anvil/document.hpp"
#include "anvil/expr.hpp"
#include "anvil/fsutil.hpp"
#include "anvil/fuzzy.hpp"
#include "anvil/geometry.hpp"
#include "anvil/journal.hpp"
#include "anvil/radial.hpp"
#include "anvil/snap.hpp"
#include "anvil/stl.hpp"
#include "anvil/tolerance.hpp"
#include "anvil/units.hpp"
#include "anvil/validate.hpp"

using namespace anvil;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {
fs::path tmpdir() {
    auto p = fs::temp_directory_path() / ("anvil_test_" + std::to_string(std::random_device{}()));
    fs::create_directories(p);
    return p;
}
Param L(double mm) { return Param{mm, {}}; }
std::shared_ptr<const Shape> eval(const NodePtr& n, double chord = 0.01) {
    EvalSettings s;
    s.chord_mm = chord;
    return evaluate(n, {}, s);
}
NodePtr magnet_base() {
    auto base = make_primitive(NodeKind::Cylinder, {{"d", L(25)}, {"h", L(3)}, {"chamfer_top", L(0.6)}});
    auto pocket = make_primitive(NodeKind::Cylinder, {{"d", L(5.1)}, {"h", L(2.2)}});
    return make_boolean(BoolOp::Difference, base, pocket);
}
Mesh cube_mesh(double s, Vec3 off = {}) {
    auto sh = eval(make_primitive(NodeKind::Box, {{"x", L(s)}, {"y", L(s)}, {"z", L(s)}}));
    Mesh m = *sh->mesh;
    for (auto& p : m.V) p = p + off;
    return m;
}
}  // namespace

// ------------------------------------------------------------------ units
TEST_CASE("inch roundtrip 25.4 mm <-> 1 in within tolerance", "[units]") {
    using namespace units;
    static_assert(std::is_same_v<decltype(mm + inch), Length>);
    REQUIRE(tol::close((1.0 * inch).value(), 25.4));
    REQUIRE(tol::close(25.4 / kMmPerInch, 1.0));
    std::mt19937_64 rng(1);
    std::uniform_real_distribution<double> u(-1e4, 1e4);
    for (int i = 0; i < 10000; ++i) {
        double x = u(rng);
        REQUIRE(tol::close((x / kMmPerInch) * kMmPerInch, x));
    }
    REQUIRE(format_length(25.4, LengthUnit::Inch) == "1 in");
    REQUIRE(format_length(128, LengthUnit::Centimeter) == "12.8 cm");
    REQUIRE(format_length(0.6, LengthUnit::Millimeter) == "0.6 mm");
}

TEST_CASE("repr_double round-trips", "[units]") {
    std::mt19937_64 rng(7);
    std::uniform_real_distribution<double> u(-1e6, 1e6);
    for (int i = 0; i < 1000; ++i) {
        double x = u(rng);
        REQUIRE(std::stod(repr_double(x)) == x);
    }
    REQUIRE(repr_double(25.0) == "25");
    REQUIRE(repr_double(0.6) == "0.6");
}

// ------------------------------------------------------------------ expressions
TEST_CASE("expression fields", "[expr]") {
    REQUIRE(eval_length_mm("12.5 cm + 3 mm") == Approx(128.0));
    REQUIRE(eval_length_mm("1/2 mm") == Approx(0.5));
    REQUIRE(eval_length_mm("1 in") == 25.4);
    REQUIRE(eval_length_mm("2\"") == 50.8);
    REQUIRE(eval_length_mm("25") == 25.0);
    REQUIRE(eval_length_mm("25", {}, LengthUnit::Centimeter) == 250.0);
    REQUIRE(eval_length_mm("3 \xc2\xb5m") == Approx(0.003));
    REQUIRE(eval_length_mm("sqrt((3 mm)^2 + (4 mm)^2)") == Approx(5.0));
    REQUIRE(eval_length_mm("-2.5e1 mm") == -25.0);
    VariableResolver vars = [](std::string_view n) -> std::optional<RtQuantity> {
        if (n == "r") return RtQuantity{12.5, dims::kLength};
        return std::nullopt;
    };
    REQUIRE(eval_length_mm("2*r", vars) == 25.0);
    REQUIRE(eval_length_mm("2 r + 1 mm", vars) == 26.0);
    auto a = eval_expr("90 deg");
    REQUIRE(a.dim == dims::kAngle);
    REQUIRE(a.value == Approx(std::numbers::pi / 2));
    REQUIRE(eval_expr("sin(30 deg)").value == Approx(0.5));
}

TEST_CASE("expression errors are precise", "[expr]") {
    auto msg = [](const char* s) {
        try {
            eval_length_mm(s);
        } catch (const ExprError& e) {
            return std::string(e.what());
        }
        return std::string();
    };
    REQUIRE(msg("3 mm + 2 deg") == "cannot add Angle to Length");
    REQUIRE(msg("2*q") == "unknown name 'q'");
    REQUIRE(msg("(3 mm") == "missing ')'");
    REQUIRE(msg("3 mm^2") == "expected a Length, got Length^2");
    REQUIRE(msg("1/0") == "division by zero");
    REQUIRE(msg("") == "unexpected end of expression");
    REQUIRE(expr_identifiers("2*r + w mm") == std::vector<std::string>{"r", "w"});
}

// ------------------------------------------------------------------ tessellation
TEST_CASE("chord formula n = ceil(pi/acos(1-delta/r)), multiple of 4", "[geom]") {
    int raw = static_cast<int>(std::ceil(std::numbers::pi / std::acos(1 - 0.01 / 12.5)));
    REQUIRE(raw == 79);
    REQUIRE(circle_segments(12.5, 0.01) == 80);
    auto sag = [](double r, int n) { return r * (1 - std::cos(std::numbers::pi / n)); };
    for (double r : {0.5, 3.0, 12.5, 100.0})
        for (double d : {1e-3, 1e-2, 0.1}) {
            int n = circle_segments(r, d);
            REQUIRE(sag(r, n) <= d * (1 + 1e-12));
            REQUIRE(n % 4 == 0);
        }
}

TEST_CASE("primitives are valid closed manifolds with exact polygonal volume", "[geom]") {
    SECTION("box") {
        auto s = eval(make_primitive(NodeKind::Box, {{"x", L(2)}, {"y", L(3)}, {"z", L(4)}}));
        auto r = validate(*s->mesh);
        REQUIRE(r.ok);
        REQUIRE(r.volume == Approx(24.0).epsilon(1e-12));
        REQUIRE(r.area == Approx(52.0).epsilon(1e-12));
    }
    SECTION("cylinder: V = (n/2) r^2 sin(2pi/n) h") {
        auto s = eval(make_primitive(NodeKind::Cylinder, {{"d", L(25)}, {"h", L(3)}}));
        auto r = validate(*s->mesh);
        REQUIRE(r.ok);
        int n = circle_segments(12.5, 0.01);
        double v = n / 2.0 * 12.5 * 12.5 * std::sin(2 * std::numbers::pi / n) * 3;
        REQUIRE(r.volume == Approx(v).epsilon(1e-12));
        REQUIRE(r.shells.size() == 1);
        REQUIRE(r.shells[0].genus == 0);
    }
    SECTION("chamfered cylinder volume (frustum of polygonal prism)") {
        auto s = eval(make_primitive(NodeKind::Cylinder, {{"d", L(25)}, {"h", L(3)}, {"chamfer_top", L(0.6)}}));
        auto r = validate(*s->mesh);
        REQUIRE(r.ok);
        int n = circle_segments(12.5, 0.01);
        double k = n / 2.0 * std::sin(2 * std::numbers::pi / n);  // area = k r^2
        double R = 12.5, c = 0.6;
        double v = k * R * R * (3 - c) + k * c / 3 * (R * R + R * (R - c) + (R - c) * (R - c));
        REQUIRE(r.volume == Approx(v).epsilon(1e-12));
    }
    SECTION("torus has genus 1 and converges with order 2") {
        double R = 20, rr = 5, Vex = 2 * std::numbers::pi * std::numbers::pi * R * rr * rr;
        std::vector<double> err, nseg;
        for (double chord : {0.08, 0.02, 0.005}) {
            nseg.push_back(circle_segments(rr, chord / 2));
            auto s = eval(make_primitive(NodeKind::Torus, {{"R", L(R)}, {"r", L(rr)}}), chord);
            auto rep = validate(*s->mesh, {false});
            REQUIRE(rep.ok);
            REQUIRE(rep.shells[0].genus == 1);
            err.push_back(std::abs(rep.volume - Vex) / Vex);
            REQUIRE(err.back() < 4.0 / 3.0 * chord / rr * 1.5);  // rel. error ~ (4/3) delta/r
        }
        // error ~ n^-2 (order 2 in the edge length h ~ 1/n)
        for (int i = 0; i < 2; ++i)
            REQUIRE(std::log(err[i] / err[i + 1]) / std::log(nseg[i + 1] / nseg[i]) == Approx(2.0).margin(0.1));
    }
    SECTION("sphere") {
        auto s = eval(make_primitive(NodeKind::Sphere, {{"d", L(20)}}), 0.01);
        auto r = validate(*s->mesh);
        REQUIRE(r.ok);
        REQUIRE(r.volume == Approx(4.0 / 3 * std::numbers::pi * 1000).epsilon(3 * 0.01 / 10));
        // chord guarantee: every face center deviates from the sphere by <= delta
        double worst = 0;
        for (const auto& t : s->mesh->F) {
            Vec3 c = (s->mesh->V[t[0]] + s->mesh->V[t[1]] + s->mesh->V[t[2]]) / 3.0;
            worst = std::max(worst, 10 - norm(c));
        }
        REQUIRE(worst <= 0.01);
    }
}

TEST_CASE("parameter validation names the problem", "[geom]") {
    auto bad = make_primitive(NodeKind::Cylinder, {{"d", L(10)}, {"h", L(1)}, {"chamfer_top", L(1)}});
    REQUIRE_THROWS_WITH(eval(bad), "Cylinder: chamfer_top + chamfer_bottom (1 mm) must be smaller than h (1 mm)");
    REQUIRE_THROWS_WITH(make_primitive(NodeKind::Box, {{"w", L(1)}}),
                        "Box has no parameter 'w' (valid: x, y, z)");
    auto neg = make_primitive(NodeKind::Sphere, {{"d", L(-1)}});
    REQUIRE_THROWS_WITH(eval(neg), "Sphere.d must be > 0 (is -1 mm)");
    auto e = make_primitive(NodeKind::Sphere, {{"d", Param{0, "25"}}});
    REQUIRE_THROWS_WITH(eval(e), "parameter 'd' = \"25\" is Dimensionless, expected Length (add a unit, e.g. '25 mm')");
}

TEST_CASE("magnet base (M1 acceptance geometry) passes all checks", "[geom][acceptance]") {
    auto s = eval(magnet_base());
    REQUIRE(s->solid);
    auto r = validate(s->mesh->rounded_to_float());
    INFO(r.summary());
    REQUIRE(r.ok);
    REQUIRE(r.shells.size() == 1);
    REQUIRE(r.shells[0].genus == 0);
    // volume = chamfered disc - pocket (polygonal)
    int n1 = circle_segments(12.5, 0.01), n2 = circle_segments(2.55, 0.01);
    auto k = [](int n) { return n / 2.0 * std::sin(2 * std::numbers::pi / n); };
    double R = 12.5, c = 0.6;
    double vb = k(n1) * (R * R * (3 - c) + c / 3 * (R * R + R * (R - c) + (R - c) * (R - c)));
    double vp = k(n2) * 2.55 * 2.55 * 2.2;
    REQUIRE(r.volume == Approx(vb - vp).epsilon(1e-6));
}

TEST_CASE("evaluation is deterministic (bit-identical STL)", "[geom][determinism]") {
    std::string h0;
    for (int i = 0; i < 5; ++i) {
        auto s = eval(magnet_base());
        std::string h = sha256_hex(encode_stl(*s->mesh, {}));
        if (i == 0) h0 = h;
        REQUIRE(h == h0);
    }
}

TEST_CASE("parametric: expression parameters resolve against variables", "[geom]") {
    auto n = make_primitive(NodeKind::Cylinder, {{"d", Param{0, "2*r"}}, {"h", L(3)}});
    VariableResolver v = [](std::string_view s) -> std::optional<RtQuantity> {
        if (s == "r") return RtQuantity{12.5, dims::kLength};
        return std::nullopt;
    };
    EvalSettings es;
    es.chord_mm = 0.01;
    auto a = evaluate(n, v, es);
    auto b = eval(make_primitive(NodeKind::Cylinder, {{"d", L(25)}, {"h", L(3)}}));
    REQUIRE(encode_stl(*a->mesh, {}) == encode_stl(*b->mesh, {}));
}

// ------------------------------------------------------------------ validation (defects)
TEST_CASE("validator names every defect precisely", "[validate]") {
    Mesh cube = cube_mesh(2);
    SECTION("open edge") {
        Mesh m = cube;
        m.F.pop_back();
        auto r = validate(m);
        REQUIRE_FALSE(r.ok);
        REQUIRE(r.counts[int(IssueKind::OpenEdge)] == 3);
    }
    SECTION("flipped triangle") {
        Mesh m = cube;
        std::swap(m.F[0][0], m.F[0][1]);
        auto r = validate(m);
        REQUIRE(r.counts[int(IssueKind::InconsistentOrient)] > 0);
    }
    SECTION("inverted shell") {
        Mesh m = cube;
        for (auto& t : m.F) std::swap(t[0], t[1]);
        auto r = validate(m);
        REQUIRE(r.counts[int(IssueKind::NegativeVolume)] == 1);
        REQUIRE(r.issues[0].message.find("normals point inward") != std::string::npos);
    }
    SECTION("degenerate triangle") {
        Mesh m = cube;
        m.V.push_back(m.V[0]);
        auto k = static_cast<std::uint32_t>(m.V.size() - 1);
        m.F.push_back({m.F[0][0], m.F[0][1], k});
        m.F.push_back({m.F[0][1], m.F[0][0], k});
        auto r = validate(m);
        REQUIRE(r.counts[int(IssueKind::Degenerate)] >= 1);
    }
    SECTION("bowtie vertex") {
        Mesh m;
        m.V = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {-1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
        m.F = {{0, 2, 1}, {0, 1, 3}, {1, 2, 3}, {0, 3, 2}, {0, 5, 4}, {0, 4, 6}, {4, 5, 6}, {0, 6, 5}};
        auto r = validate(m);
        REQUIRE(r.counts[int(IssueKind::NonManifoldVertex)] == 1);
        REQUIRE(r.issues[0].message.find("non-manifold vertex #0") == 0);
    }
    SECTION("self-intersection between two overlapping shells") {
        Mesh m = cube;
        m.append(cube_mesh(2, {0.5, 0.3, 0.2}));
        auto r = validate(m);
        REQUIRE(r.counts[int(IssueKind::SelfIntersection)] > 0);
    }
    SECTION("two disjoint cubes are fine") {
        Mesh m = cube;
        m.append(cube_mesh(2, {5, 0, 0}));
        auto r = validate(m);
        REQUIRE(r.ok);
        REQUIRE(r.shells.size() == 2);
    }
}

TEST_CASE("property: random boolean results validate", "[validate][property]") {
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> u(-6, 6), s(2, 10);
    for (int i = 0; i < 25; ++i) {
        auto a = make_primitive(NodeKind::Cylinder, {{"d", L(s(rng) * 2)}, {"h", L(s(rng))}});
        auto b = make_transform(make_primitive(NodeKind::Box, {{"x", L(s(rng))}, {"y", L(s(rng))}, {"z", L(s(rng))}}),
                                {{"tx", L(u(rng))}, {"ty", L(u(rng))}, {"tz", L(u(rng) / 2)},
                                 {"rz", Param{u(rng) / 6, {}}}});
        BoolOp op = BoolOp(i % 3);
        auto sh = eval(make_boolean(op, a, b), 0.05);
        if (sh->mesh->F.empty()) continue;
        auto r = validate(sh->mesh->rounded_to_float());
        INFO("case " << i << ": " << r.summary());
        REQUIRE(r.ok);
    }
}

// ------------------------------------------------------------------ STL
TEST_CASE("binary STL is exactly 84 + 50 N bytes and round-trips", "[stl]") {
    auto s = eval(magnet_base());
    auto dir = tmpdir();
    auto p = dir / "base.stl";
    std::string sha = write_stl(p, *s->mesh, {});
    REQUIRE(fs::file_size(p) == 84 + 50 * s->mesh->F.size());
    REQUIRE(sha == sha256_hex(read_file(p)));
    auto back = read_stl(p);
    REQUIRE_FALSE(back.ascii);
    REQUIRE(back.header.find("unit=mm") != std::string::npos);
    REQUIRE(back.mesh.F.size() == s->mesh->F.size());
    auto r = validate(back.mesh);
    REQUIRE(r.ok);
    REQUIRE(r.volume == Approx(s->mesh->signed_volume()).epsilon(1e-6));
    fs::remove_all(dir);
}

TEST_CASE("STL in inches scales by 1/25.4; ASCII parses", "[stl]") {
    Mesh m = cube_mesh(25.4);
    StlWriteOptions o;
    o.unit = LengthUnit::Inch;
    auto bin = decode_stl(encode_stl(m, o), LengthUnit::Inch);
    REQUIRE(bin.mesh.bounds().hi.x == Approx(12.7));
    o.ascii = true;
    auto txt = encode_stl(m, o);
    REQUIRE(txt.rfind("solid", 0) == 0);
    auto asc = decode_stl(txt, LengthUnit::Millimeter);
    REQUIRE(asc.ascii);
    REQUIRE(asc.mesh.bounds().hi.x == Approx(0.5));  // file is in inches, read as mm
    REQUIRE(validate(asc.mesh).ok);
}

TEST_CASE("SHA-256 known answer", "[stl]") {
    REQUIRE(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

// ------------------------------------------------------------------ document / undo
TEST_CASE("transactions, undo, redo", "[doc]") {
    Document d;
    d.begin("show(cylinder())", "ui");
    std::string n = d.add_object("", make_primitive(NodeKind::Cylinder, {}));
    REQUIRE(d.commit());
    REQUIRE(n == "Cylinder1");
    d.begin("edit", "ui");
    d.edit_params(n, "", {{"d", L(25)}});
    REQUIRE(d.commit());
    REQUIRE(d.state()->find(n)->root->params.at("d").value == 25);
    d.begin("noop", "console");
    REQUIRE_FALSE(d.commit());  // no change -> no undo step
    REQUIRE(d.history().size() == 2);
    REQUIRE(d.undo());
    REQUIRE(d.state()->find(n)->root->params.at("d").value == 20);
    REQUIRE(d.redo());
    REQUIRE(d.state()->find(n)->root->params.at("d").value == 25);
    d.begin("failing", "script");
    d.remove_object(n);
    d.rollback();
    REQUIRE(d.state()->find(n));
    REQUIRE_THROWS(d.add_object("x", nullptr));  // outside txn
}

TEST_CASE("document serializes to an exact Python script", "[doc]") {
    Document d;
    d.begin("x", "script");
    d.set_var("r", Param{12.5, {}}, dims::kLength);
    d.add_object("Base", make_boolean(BoolOp::Difference,
                                      make_primitive(NodeKind::Cylinder, {{"d", Param{0, "2*r"}}, {"h", L(3)},
                                                                          {"chamfer_top", L(0.6)}}),
                                      make_transform(make_primitive(NodeKind::Cylinder, {{"d", L(5.1)}, {"h", L(2.2)}}),
                                                     {{"rz", Param{std::numbers::pi / 4, {}}}})));
    d.commit();
    std::string py = d.state()->to_python();
    INFO(py);
    REQUIRE(py.find("var(\"r\", 12.5*mm)") != std::string::npos);
    REQUIRE(py.find("show((cylinder(d=\"2*r\", h=3*mm, chamfer_top=0.6*mm) - transform(cylinder(d=5.1*mm, h=2.2*mm), "
                    "rz=45*deg)), name=\"Base\")") != std::string::npos);
}

TEST_CASE("variables: recursion is detected", "[doc]") {
    Document d;
    d.begin("x", "script");
    d.set_var("a", Param{1, {}}, dims::kLength);
    REQUIRE_THROWS(d.set_var("a", Param{0, "2*a"}, dims::kLength));
    REQUIRE_THROWS(d.set_var("mm", Param{1, {}}, dims::kLength));
}

// ------------------------------------------------------------------ journal
TEST_CASE("journal: write-ahead, recovery, torn tail", "[journal]") {
    auto dir = tmpdir();
    auto p = dir / "journal.py";
    {
        Journal j(p);
        auto a = j.begin_txn("ui", "show(cylinder(d=25*mm, h=3*mm))");
        j.mark_ok(a);
        auto b = j.begin_txn("console", "edit(\"Cylinder1\", d=26*mm)\n#@end 99 tricky");
        j.mark_ok(b);
        j.mark_undo();
        j.begin_txn("script", "crash_here()");  // no ok: process died during execution
    }
    auto c = Journal::read(p);
    REQUIRE(c.actions.size() == 4);
    REQUIRE(c.actions[0].ok);
    REQUIRE(c.actions[1].source == "edit(\"Cylinder1\", d=26*mm)\n#@end 99 tricky");
    REQUIRE(c.actions[2].kind == JournalAction::Undo);
    REQUIRE_FALSE(c.actions[3].ok);
    REQUIRE_FALSE(c.clean_close);
    // torn write: cut the file in the middle of the last record
    std::string data = read_file(p);
    write_file_atomic(p, data.substr(0, data.size() - 8));
    auto t = Journal::read(p);
    REQUIRE(t.truncated_tail);
    REQUIRE(t.actions.size() == 3);
    {
        Journal j(p);  // reopening continues the sequence
        REQUIRE(j.begin_txn("ui", "x") == 3);  // torn record 3 is reused
    }
    fs::remove_all(dir);
}

// ------------------------------------------------------------------ camera
TEST_CASE("camera project/ray are consistent", "[camera]") {
    for (bool ortho : {false, true}) {
        Camera c;
        c.ortho = ortho;
        c.width = 1920, c.height = 1080;
        c.target = {3, -2, 5};
        for (double px : {0.0, 333.0, 960.0, 1919.0})
            for (double py : {0.0, 540.0, 1079.0}) {
                Ray r = c.ray(px, py);
                Vec3 p = r.origin + r.dir * (ortho ? c.ortho_range() * 0.5 + 50 : 120.0);
                double x, y;
                REQUIRE(c.project(p, x, y));
                REQUIRE(x == Approx(px).margin(1e-6));
                REQUIRE(y == Approx(py).margin(1e-6));
            }
    }
    Camera top;
    top.look_from({0, 0, 1});
    REQUIRE(top.forward().z == Approx(-1));
    REQUIRE(top.up().y == Approx(1));  // +Y up on screen in top view
}

// ------------------------------------------------------------------ BVH
TEST_CASE("property: BVH raycast equals brute force", "[bvh][property]") {
    auto s = eval(magnet_base(), 0.05);
    Bvh bvh(*s->mesh);
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> u(-1, 1);
    for (int i = 0; i < 300; ++i) {
        Vec3 o{u(rng) * 30, u(rng) * 30, 20 + u(rng) * 5};
        Vec3 d = normalized(Vec3{u(rng) * 0.6, u(rng) * 0.6, -1});
        auto h = bvh.raycast(o, d);
        double best = INFINITY;
        for (const auto& t : s->mesh->F) {
            Vec3 p0 = s->mesh->V[t[0]], e1 = s->mesh->V[t[1]] - p0, e2 = s->mesh->V[t[2]] - p0;
            Vec3 pv = cross(d, e2);
            double det = dot(e1, pv);
            if (det == 0) continue;
            Vec3 tv = o - p0;
            double uu = dot(tv, pv) / det, vv = dot(d, cross(tv, e1)) / det, tt = dot(e2, cross(tv, e1)) / det;
            if (uu >= 0 && vv >= 0 && uu + vv <= 1 && tt > 0) best = std::min(best, tt);
        }
        if (std::isinf(best)) REQUIRE_FALSE(h);
        else {
            REQUIRE(h);
            REQUIRE(h->t == Approx(best).epsilon(1e-12));
        }
    }
}

// ------------------------------------------------------------------ snapping
TEST_CASE("snapping: vertex beats surface, hysteresis, Alt disables", "[snap]") {
    auto s = eval(make_primitive(NodeKind::Box, {{"x", L(20)}, {"y", L(20)}, {"z", L(20)}}));
    SnapScene scene;
    scene.objects.push_back(make_snap_object("Box1", s->mesh, {}));
    SnapEngine eng;
    SnapQuery q;
    q.cam.width = 1000, q.cam.height = 800;
    q.cam.target = {0, 0, 10};
    q.cam.distance = 120;
    double cx, cy;
    REQUIRE(q.cam.project({10, 10, 20}, cx, cy));  // a corner
    q.mx = cx + 3, q.my = cy + 2;
    auto r = eng.update(scene, q);
    REQUIRE(r.type == SnapType::Vertex);
    REQUIRE(norm(r.point - Vec3{10, 10, 20}) < 1e-9);
    q.disabled = true;
    REQUIRE(eng.update(scene, q).type != SnapType::Vertex);
    q.disabled = false;
    // hysteresis: halfway between corner and edge midpoint the corner stays
    double mx2, my2;
    q.cam.project({10, 0, 20}, mx2, my2);
    eng.reset();
    q.mx = cx, q.my = cy;
    REQUIRE(eng.update(scene, q).type == SnapType::Vertex);
    q.mx = cx + (mx2 - cx) * 0.02, q.my = cy + (my2 - cy) * 0.02;
    REQUIRE(eng.update(scene, q).type == SnapType::Vertex);
    // the origin is a snap target
    double ox, oy;
    q.cam.target = {0, 0, 0};
    q.cam.yaw = 0.3;
    q.cam.project({0, 0, 0}, ox, oy);
    SnapScene empty;
    eng.reset();
    q.mx = ox + 2, q.my = oy - 1;
    auto o = eng.update(empty, q);
    REQUIRE(o.type == SnapType::Origin);
}

TEST_CASE("snapping: cylinder center and tangent from base point", "[snap]") {
    auto node = make_primitive(NodeKind::Cylinder, {{"d", L(20)}, {"h", L(5)}});
    auto s = eval(node);
    SnapScene scene;
    scene.objects.push_back(make_snap_object("C", s->mesh, analytic_features(node, {})));
    SnapEngine eng;
    SnapQuery q;
    q.cam.width = 1000, q.cam.height = 800;
    q.cam.look_from({0, 0, 1});
    q.cam.distance = 100;
    q.cam.target = {0, 0, 5};
    double x, y;
    q.cam.project({0, 0, 5}, x, y);
    q.mx = x + 4, q.my = y;
    auto r = eng.update(scene, q);
    REQUIRE(r.type == SnapType::Center);
    REQUIRE(norm(r.point - Vec3{0, 0, 5}) < 1e-9);
    // tangent point from (30, 0, 5): angle acos(10/30)
    q.base = Vec3{30, 0, 5};
    double a = std::acos(10.0 / 30);
    Vec3 tp{10 * std::cos(a), 10 * std::sin(a), 5};
    q.cam.project(tp, x, y);
    q.mx = x + 1, q.my = y + 1;
    eng.reset();
    eng.settings.types[int(SnapType::Vertex)].enabled = false;
    eng.settings.types[int(SnapType::Midpoint)].enabled = false;
    auto t = eng.update(scene, q);
    REQUIRE(t.type == SnapType::Tangent);
    REQUIRE(norm(t.point - tp) < 1e-9);
}

TEST_CASE("ViewCube: 26 directions and region classification", "[snap]") {
    REQUIRE(cube_directions().size() == 26);
    REQUIRE(classify_cube_hit({0.1, -0.2, 1}) == std::array<int, 3>{0, 0, 1});
    REQUIRE(classify_cube_hit({0.9, 0.0, 1}) == std::array<int, 3>{1, 0, 1});
    REQUIRE(classify_cube_hit({-0.9, 0.95, 1}) == std::array<int, 3>{-1, 1, 1});
    REQUIRE(classify_cube_hit({1, 0.5, -0.95}) == std::array<int, 3>{1, 0, -1});
}

// ------------------------------------------------------------------ radial menu
TEST_CASE("radial sectors by angle with dead zone", "[radial]") {
    REQUIRE(radial_sector(5, 0, 8, 12) == -1);
    REQUIRE(radial_sector(50, 0, 8, 12) == 0);     // east
    REQUIRE(radial_sector(0, -50, 8, 12) == 2);    // north (screen y up)
    REQUIRE(radial_sector(-50, 0, 8, 12) == 4);    // west
    REQUIRE(radial_sector(0, 50, 8, 12) == 6);     // south
    REQUIRE(radial_sector(50, -50, 8, 12) == 1);   // north-east
    REQUIRE(radial_sector(50, -19, 8, 12) == 0);   // 20.8 deg < 22.5 deg
    REQUIRE(radial_sector(50, -22, 8, 12) == 1);   // 23.7 deg > 22.5 deg
    REQUIRE(radial_fitts_id(8) == Approx(1.1847).margin(1e-4));
}

TEST_CASE("radial gesture: flick without moves, submenu, cancel", "[radial]") {
    auto rings = [](const std::vector<int>& path) {
        std::vector<RadialItem> r(8);
        if (path.empty()) r[2].submenu = true, r[5].enabled = false;
        return r;
    };
    RadialGesture g;
    g.begin(100, 100, rings);
    auto sel = g.end(160, 100);  // release before any move event was delivered
    REQUIRE(sel);
    REQUIRE(*sel == std::vector<int>{0});
    g.begin(100, 100, rings);
    g.move(100, 40);
    g.move(100, -20);  // 120 px north -> opens submenu 2 centered here
    REQUIRE(g.path() == std::vector<int>{2});
    sel = g.end(40, -20);  // west in the submenu
    REQUIRE(*sel == std::vector<int>{2, 4});
    g.begin(0, 0, rings);
    REQUIRE_FALSE(g.end(3, 3));  // dead zone = cancel
    g.begin(0, 0, rings);
    REQUIRE_FALSE(g.end(-40, 40));  // disabled slot 5 (south-west)
}

// ------------------------------------------------------------------ fuzzy
TEST_CASE("fuzzy search ranks word starts", "[fuzzy]") {
    std::vector<FuzzyEntry> e{{"a", "Create Cylinder"}, {"b", "Boolean Subtract"}, {"c", "Export STL"},
                              {"d", "Create Cone"}, {"e", "Toggle Snapping"}};
    REQUIRE(e[fuzzy_rank("cyl", e)[0]].id == "a");
    REQUIRE(e[fuzzy_rank("bs", e)[0]].id == "b");
    REQUIRE(e[fuzzy_rank("stl", e)[0]].id == "c");
    REQUIRE(fuzzy_rank("xyz", e).empty());
    e[3].usage = 1000;
    REQUIRE(e[fuzzy_rank("c", e)[0]].id == "d");
}
