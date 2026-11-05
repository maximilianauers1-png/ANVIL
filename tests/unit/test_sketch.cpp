// Sketch solver: convergence, DOF analysis, conflicts, profiles.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <numbers>

#include "anvil/sketch.hpp"

using namespace anvil;
using Catch::Approx;

namespace {
Param L(double v) { return {v, {}}; }
// Rectangle with 4 lines, corners P1..P4 (deliberately skewed start values).
Sketch rect_sketch() {
    Sketch s;
    s.points = {{"P1", {0.3, -0.2}}, {"P2", {38, 1.5}}, {"P3", {41, 22}}, {"P4", {-1, 19}}};
    s.curves = {{"L1", {CurveKind::Line, {"P1", "P2"}}}, {"L2", {CurveKind::Line, {"P2", "P3"}}},
                {"L3", {CurveKind::Line, {"P3", "P4"}}}, {"L4", {CurveKind::Line, {"P4", "P1"}}}};
    s.constraints = {{"K1", {ConstraintKind::Horizontal, {"L1"}}}, {"K2", {ConstraintKind::Vertical, {"L2"}}},
                     {"K3", {ConstraintKind::Horizontal, {"L3"}}}, {"K4", {ConstraintKind::Vertical, {"L4"}}}};
    return s;
}
}  // namespace

TEST_CASE("rectangle: DOF counting, then fully constrained", "[sketch]") {
    Sketch s = rect_sketch();
    auto [a, st] = solve_sketch(s, {});
    REQUIRE(st.converged);
    REQUIRE(st.dof == 4);  // 8 coordinates - 4 independent constraints: x, y, width, height
    s.constraints["K5"] = {ConstraintKind::LineLength, {"L1"}, L(40)};
    s.constraints["K6"] = {ConstraintKind::LineLength, {"L2"}, L(20)};
    s.constraints["K7"] = {ConstraintKind::Fix, {"P1"}, L(0), L(0)};
    auto [b, st2] = solve_sketch(s, {});
    REQUIRE(st2.converged);
    REQUIRE(st2.dof == 0);
    REQUIRE(st2.message == "fully constrained");
    REQUIRE(b.points["P3"].x == Approx(40).margin(1e-9));
    REQUIRE(b.points["P3"].y == Approx(20).margin(1e-9));
    for (const auto& [id, state] : st2.entity_state) REQUIRE(state == "fixed");
}

TEST_CASE("under-determined entities are reported as free", "[sketch]") {
    Sketch s = rect_sketch();
    s.constraints["K7"] = {ConstraintKind::Fix, {"P1"}, L(0), L(0)};
    s.constraints["K5"] = {ConstraintKind::LineLength, {"L1"}, L(40)};
    auto [b, st] = solve_sketch(s, {});
    REQUIRE(st.dof == 1);                      // height still free
    REQUIRE(st.entity_state["P1"] == "fixed");
    REQUIRE(st.entity_state["P2"] == "fixed");
    REQUIRE(st.entity_state["P3"] == "free");
    REQUIRE(st.entity_state["L2"] == "free");
}

TEST_CASE("conflicting and redundant constraints are identified", "[sketch]") {
    Sketch s = rect_sketch();
    s.constraints["K5"] = {ConstraintKind::LineLength, {"L1"}, L(40)};
    s.constraints["K6"] = {ConstraintKind::LineLength, {"L3"}, L(30)};  // opposite side of a rectangle: conflict
    auto [b, st] = solve_sketch(s, {});
    REQUIRE_FALSE(st.converged);
    REQUIRE(st.constraint_state["K6"] == "conflict");
    REQUIRE(st.message.rfind("constraints conflict", 0) == 0);

    Sketch r = rect_sketch();
    r.constraints["K5"] = {ConstraintKind::LineLength, {"L1"}, L(40)};
    r.constraints["K6"] = {ConstraintKind::LineLength, {"L3"}, L(40)};  // consistent but redundant
    auto [c, st2] = solve_sketch(r, {});
    REQUIRE(st2.converged);
    REQUIRE(st2.constraint_state["K6"] == "redundant");
}

TEST_CASE("parametric dimensions follow variables", "[sketch]") {
    Sketch s = rect_sketch();
    s.constraints["K5"] = {ConstraintKind::LineLength, {"L1"}, Param{0, "2*w"}};
    s.constraints["K7"] = {ConstraintKind::Fix, {"P1"}, L(0), L(0)};
    VariableResolver v = [](std::string_view n) -> std::optional<RtQuantity> {
        if (n == "w") return RtQuantity{17.5, dims::kLength};
        return std::nullopt;
    };
    auto [b, st] = solve_sketch(s, v);
    REQUIRE(b.points["P2"].x == Approx(35).margin(1e-9));
}

TEST_CASE("circles, tangency, angles", "[sketch]") {
    Sketch s;
    s.points = {{"P1", {0, 0}}, {"P2", {10, 0}}, {"P3", {5, 4}}};
    s.curves = {{"L1", {CurveKind::Line, {"P1", "P2"}}}, {"C1", {CurveKind::Circle, {"P3"}, 2.0}}};
    s.constraints = {{"K1", {ConstraintKind::Fix, {"P1"}, L(0), L(0)}},
                     {"K2", {ConstraintKind::Horizontal, {"L1"}}},
                     {"K3", {ConstraintKind::LineLength, {"L1"}, L(10)}},
                     {"K4", {ConstraintKind::Radius, {"C1"}, L(3)}},
                     {"K5", {ConstraintKind::Tangent, {"L1", "C1"}}},
                     {"K6", {ConstraintKind::HDistance, {"P1", "P3"}, L(5)}}};
    auto [b, st] = solve_sketch(s, {});
    REQUIRE(st.converged);
    REQUIRE(st.dof == 0);
    REQUIRE(std::abs(b.points["P3"].y) == Approx(3).margin(1e-9));
    REQUIRE(b.curves["C1"].radius == Approx(3).margin(1e-9));
}

TEST_CASE("profiles: rectangle with a hole and an arc slot", "[sketch][profile]") {
    Sketch s = rect_sketch();
    s.constraints["K5"] = {ConstraintKind::LineLength, {"L1"}, L(40)};
    s.constraints["K6"] = {ConstraintKind::LineLength, {"L2"}, L(20)};
    s.constraints["K7"] = {ConstraintKind::Fix, {"P1"}, L(0), L(0)};
    s.points["P9"] = {20, 10};
    s.curves["C1"] = {CurveKind::Circle, {"P9"}, 3.0};
    auto [b, st] = solve_sketch(s, {});
    auto faces = sketch_profiles(b);
    REQUIRE(faces.size() == 1);
    REQUIRE(faces[0].holes.size() == 1);
    REQUIRE(std::abs(faces[0].outer.area) == Approx(800).epsilon(1e-9));  // solver tolerance 1e-9 mm
    REQUIRE(std::abs(faces[0].holes[0].area) == Approx(std::numbers::pi * 9).epsilon(1e-9));

    // stadium: two lines + two half circles (area = 2 r L + pi r^2)
    Sketch d;
    d.points = {{"A", {0, -2}}, {"B", {10, -2}}, {"C", {10, 2}}, {"D", {0, 2}}, {"M1", {10, 0}}, {"M2", {0, 0}}};
    d.curves = {{"L1", {CurveKind::Line, {"A", "B"}}}, {"A1", {CurveKind::Arc, {"M1", "B", "C"}}},
                {"L2", {CurveKind::Line, {"C", "D"}}}, {"A2", {CurveKind::Arc, {"M2", "D", "A"}}}};
    auto f2 = sketch_profiles(d);
    REQUIRE(f2.size() == 1);
    REQUIRE(f2[0].outer.area == Approx(2 * 2 * 10 + std::numbers::pi * 4).epsilon(1e-12));
}

TEST_CASE("open profile is named precisely", "[sketch][profile]") {
    Sketch s;
    s.points = {{"P1", {0, 0}}, {"P2", {10, 0}}, {"P3", {10, 5}}};
    s.curves = {{"L1", {CurveKind::Line, {"P1", "P2"}}}, {"L2", {CurveKind::Line, {"P2", "P3"}}}};
    REQUIRE_THROWS_WITH(sketch_profiles(s),
                        "profile is open at (0, 0): 1 curve end(s) meet there, a closed profile needs exactly 2");
}
