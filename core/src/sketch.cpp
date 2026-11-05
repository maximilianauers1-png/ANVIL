#include "anvil/sketch.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <set>
#include <sstream>

#include "anvil/tolerance.hpp"

namespace anvil {

namespace {
constexpr double kSolveTolMm = 1e-9;        // converged when every residual is below this
constexpr int kMaxIterations = 200;
constexpr double kRankRel = 1e-9;           // singular values below rel*sigma_max count as zero
constexpr double kNullComponent = 1e-6;     // null-space row norm marking a free variable
constexpr double kLoopJoinMm = 1e-6;        // endpoints closer than this form a profile vertex
constexpr double kNewtonMuRel = 1e-12;      // regularisation of J J^T, relative to its largest diagonal entry
constexpr double kMinStep = 1e-4;           // smallest backtracking fraction of the Newton step
constexpr double kArmijo = 1e-4;            // sufficient decrease: ||f_new||^2 < (1 - kArmijo*alpha) ||f||^2
}  // namespace

// ---------------------------------------------------------------- names
const char* constraint_name(ConstraintKind k) {
    switch (k) {
        case ConstraintKind::Coincident: return "coincident";
        case ConstraintKind::Horizontal: return "horizontal";
        case ConstraintKind::Vertical: return "vertical";
        case ConstraintKind::Distance: return "distance";
        case ConstraintKind::HDistance: return "hdistance";
        case ConstraintKind::VDistance: return "vdistance";
        case ConstraintKind::LineLength: return "length";
        case ConstraintKind::Radius: return "radius";
        case ConstraintKind::Diameter: return "diameter";
        case ConstraintKind::LineAngle: return "angle";
        case ConstraintKind::Parallel: return "parallel";
        case ConstraintKind::Perpendicular: return "perpendicular";
        case ConstraintKind::Tangent: return "tangent";
        case ConstraintKind::Equal: return "equal";
        case ConstraintKind::Fix: return "fix";
        case ConstraintKind::PointOnLine: return "point_on_line";
        case ConstraintKind::PointOnCircle: return "point_on_circle";
        case ConstraintKind::Midpoint: return "midpoint";
        case ConstraintKind::Symmetric: return "symmetric";
        case ConstraintKind::Concentric: return "concentric";
    }
    return "?";
}

std::optional<ConstraintKind> parse_constraint(const std::string& s) {
    for (int i = 0; i <= int(ConstraintKind::Concentric); ++i)
        if (s == constraint_name(ConstraintKind(i))) return ConstraintKind(i);
    return std::nullopt;
}

bool constraint_has_value(ConstraintKind k) {
    switch (k) {
        case ConstraintKind::Distance: case ConstraintKind::HDistance: case ConstraintKind::VDistance:
        case ConstraintKind::LineLength: case ConstraintKind::Radius: case ConstraintKind::Diameter:
        case ConstraintKind::LineAngle: case ConstraintKind::Fix:
            return true;
        default:
            return false;
    }
}

const char* plane_name(SketchPlane p) { return p == SketchPlane::XY ? "XY" : p == SketchPlane::XZ ? "XZ" : "YZ"; }
std::optional<SketchPlane> parse_plane(const std::string& s) {
    if (s == "XY") return SketchPlane::XY;
    if (s == "XZ") return SketchPlane::XZ;
    if (s == "YZ") return SketchPlane::YZ;
    return std::nullopt;
}

std::string Sketch::next_id(char prefix) const {
    for (int i = 1;; ++i) {
        std::string id = std::string(1, prefix) + std::to_string(i);
        if (!points.count(id) && !curves.count(id) && !constraints.count(id)) return id;
    }
}

void Sketch::frame(Vec3& o, Vec3& u, Vec3& v, Vec3& n) const {
    switch (plane) {
        case SketchPlane::XY: u = {1, 0, 0}, v = {0, 1, 0}; break;
        case SketchPlane::XZ: u = {1, 0, 0}, v = {0, 0, 1}; break;   // seen from -Y (front view)
        case SketchPlane::YZ: u = {0, 1, 0}, v = {0, 0, 1}; break;   // seen from +X (right view)
    }
    n = cross(u, v);
    o = n * offset;
}

Vec3 Sketch::to_world(double x, double y) const {
    Vec3 o, u, v, n;
    frame(o, u, v, n);
    return o + u * x + v * y;
}

// ---------------------------------------------------------------- solver
namespace {

struct Layout {
    std::map<std::string, int> point_index;   // -> index of x (y = x+1)
    std::map<std::string, int> radius_index;  // circle id -> index
    int n = 0;
};

Layout make_layout(const Sketch& s) {
    Layout L;
    for (const auto& [id, p] : s.points) L.point_index[id] = L.n, L.n += 2;
    for (const auto& [id, c] : s.curves)
        if (c.kind == CurveKind::Circle) L.radius_index[id] = L.n++;
    return L;
}

struct ResolvedConstraint {
    std::string id;
    ConstraintKind kind;
    std::vector<std::string> refs;
    double v1 = 0, v2 = 0;
};

using Vec = Eigen::VectorXd;

class System {
  public:
    System(const Sketch& s, const Layout& L, std::vector<ResolvedConstraint> cons) : s_(s), L_(L), cons_(std::move(cons)) {
        // implicit arc constraints: |start - c| = |end - c|
        for (const auto& [id, c] : s.curves)
            if (c.kind == CurveKind::Arc) arcs_.push_back(id);
        for (const auto& c : cons_) owner_.insert(owner_.end(), rows(c), c.id);
        for (const auto& a : arcs_) owner_.push_back("arc:" + a);
    }
    int m() const { return int(owner_.size()); }
    const std::vector<std::string>& owner() const { return owner_; }

    Vec residual(const Vec& q) const {
        Vec r(m());
        int k = 0;
        for (const auto& c : cons_) eval(c, q, r, k);
        for (const auto& a : arcs_) {
            const auto& cu = s_.curves.at(a);
            double c0x, c0y, sx, sy, ex, ey;
            pt(q, cu.pts[0], c0x, c0y), pt(q, cu.pts[1], sx, sy), pt(q, cu.pts[2], ex, ey);
            r[k++] = std::hypot(sx - c0x, sy - c0y) - std::hypot(ex - c0x, ey - c0y);
        }
        return r;
    }

    Eigen::MatrixXd jacobian(const Vec& q) const {
        // central differences; residuals are smooth in the region of interest
        Eigen::MatrixXd J(m(), q.size());
        Vec qq = q;
        for (int j = 0; j < q.size(); ++j) {
            double h = 1e-7 * std::max(1.0, std::abs(q[j]));
            qq[j] = q[j] + h;
            Vec fp = residual(qq);
            qq[j] = q[j] - h;
            Vec fm = residual(qq);
            qq[j] = q[j];
            J.col(j) = (fp - fm) / (2 * h);
        }
        return J;
    }

  private:
    static int rows(const ResolvedConstraint& c) {
        switch (c.kind) {
            case ConstraintKind::Coincident: case ConstraintKind::Fix: case ConstraintKind::Midpoint:
            case ConstraintKind::Symmetric: case ConstraintKind::Concentric:
                return 2;
            default:
                return 1;
        }
    }
    void pt(const Vec& q, const std::string& id, double& x, double& y) const {
        int i = L_.point_index.at(id);
        x = q[i], y = q[i + 1];
    }
    // line endpoints
    void line(const Vec& q, const std::string& id, double& ax, double& ay, double& bx, double& by) const {
        const auto& c = s_.curves.at(id);
        if (c.kind != CurveKind::Line) throw GeometryError("'" + id + "' is not a line");
        pt(q, c.pts[0], ax, ay), pt(q, c.pts[1], bx, by);
    }
    // circle/arc center + radius
    void circ(const Vec& q, const std::string& id, double& cx, double& cy, double& r) const {
        const auto& c = s_.curves.at(id);
        if (c.kind == CurveKind::Line) throw GeometryError("'" + id + "' is not a circle or arc");
        pt(q, c.pts[0], cx, cy);
        if (c.kind == CurveKind::Circle) r = q[L_.radius_index.at(id)];
        else {
            double sx, sy;
            pt(q, c.pts[1], sx, sy);
            r = std::hypot(sx - cx, sy - cy);
        }
    }
    bool is_line(const std::string& id) const { return s_.curves.count(id) && s_.curves.at(id).kind == CurveKind::Line; }

    void eval(const ResolvedConstraint& c, const Vec& q, Vec& r, int& k) const {
        auto ref = [&](std::size_t i) -> const std::string& {
            if (i >= c.refs.size()) throw GeometryError(std::string(constraint_name(c.kind)) + " " + c.id + ": missing reference");
            return c.refs[i];
        };
        double ax, ay, bx, by, px, py, qx, qy, cx, cy, rr;
        switch (c.kind) {
            case ConstraintKind::Coincident:
                pt(q, ref(0), px, py), pt(q, ref(1), qx, qy);
                r[k++] = px - qx, r[k++] = py - qy;
                break;
            case ConstraintKind::Horizontal:
                if (is_line(ref(0))) line(q, ref(0), ax, ay, bx, by);
                else pt(q, ref(0), ax, ay), pt(q, ref(1), bx, by);
                r[k++] = by - ay;
                break;
            case ConstraintKind::Vertical:
                if (is_line(ref(0))) line(q, ref(0), ax, ay, bx, by);
                else pt(q, ref(0), ax, ay), pt(q, ref(1), bx, by);
                r[k++] = bx - ax;
                break;
            case ConstraintKind::Distance:
                pt(q, ref(0), px, py), pt(q, ref(1), qx, qy);
                r[k++] = std::hypot(qx - px, qy - py) - c.v1;
                break;
            case ConstraintKind::HDistance:
                pt(q, ref(0), px, py), pt(q, ref(1), qx, qy);
                r[k++] = (qx - px) - c.v1;
                break;
            case ConstraintKind::VDistance:
                pt(q, ref(0), px, py), pt(q, ref(1), qx, qy);
                r[k++] = (qy - py) - c.v1;
                break;
            case ConstraintKind::LineLength:
                line(q, ref(0), ax, ay, bx, by);
                r[k++] = std::hypot(bx - ax, by - ay) - c.v1;
                break;
            case ConstraintKind::Radius:
                circ(q, ref(0), cx, cy, rr);
                r[k++] = rr - c.v1;
                break;
            case ConstraintKind::Diameter:
                circ(q, ref(0), cx, cy, rr);
                r[k++] = 2 * rr - c.v1;
                break;
            case ConstraintKind::LineAngle: {
                double a2x, a2y, b2x, b2y;
                line(q, ref(0), ax, ay, bx, by), line(q, ref(1), a2x, a2y, b2x, b2y);
                double ux = bx - ax, uy = by - ay, vx = b2x - a2x, vy = b2y - a2y;
                double ang = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
                r[k++] = std::remainder(ang - c.v1, 2 * std::numbers::pi);
                break;
            }
            case ConstraintKind::Parallel: case ConstraintKind::Perpendicular: {
                double a2x, a2y, b2x, b2y;
                line(q, ref(0), ax, ay, bx, by), line(q, ref(1), a2x, a2y, b2x, b2y);
                double ux = bx - ax, uy = by - ay, vx = b2x - a2x, vy = b2y - a2y;
                double nu = std::max(std::hypot(ux, uy), 1e-12), nv = std::max(std::hypot(vx, vy), 1e-12);
                r[k++] = c.kind == ConstraintKind::Parallel ? (ux * vy - uy * vx) / (nu * nv) : (ux * vx + uy * vy) / (nu * nv);
                break;
            }
            case ConstraintKind::Tangent: {
                if (is_line(ref(0)) || is_line(ref(1))) {
                    const std::string& l = is_line(ref(0)) ? ref(0) : ref(1);
                    const std::string& o = is_line(ref(0)) ? ref(1) : ref(0);
                    line(q, l, ax, ay, bx, by);
                    circ(q, o, cx, cy, rr);
                    double len = std::max(std::hypot(bx - ax, by - ay), 1e-12);
                    double d = std::abs((bx - ax) * (cy - ay) - (by - ay) * (cx - ax)) / len;
                    r[k++] = d - rr;
                } else {
                    double c2x, c2y, r2;
                    circ(q, ref(0), cx, cy, rr), circ(q, ref(1), c2x, c2y, r2);
                    r[k++] = std::hypot(c2x - cx, c2y - cy) - (rr + r2);  // external tangency
                }
                break;
            }
            case ConstraintKind::Equal: {
                if (is_line(ref(0))) {
                    double a2x, a2y, b2x, b2y;
                    line(q, ref(0), ax, ay, bx, by), line(q, ref(1), a2x, a2y, b2x, b2y);
                    r[k++] = std::hypot(bx - ax, by - ay) - std::hypot(b2x - a2x, b2y - a2y);
                } else {
                    double c2x, c2y, r2;
                    circ(q, ref(0), cx, cy, rr), circ(q, ref(1), c2x, c2y, r2);
                    r[k++] = rr - r2;
                }
                break;
            }
            case ConstraintKind::Fix:
                pt(q, ref(0), px, py);
                r[k++] = px - c.v1, r[k++] = py - c.v2;
                break;
            case ConstraintKind::PointOnLine: {
                pt(q, ref(0), px, py), line(q, ref(1), ax, ay, bx, by);
                double len = std::max(std::hypot(bx - ax, by - ay), 1e-12);
                r[k++] = ((bx - ax) * (py - ay) - (by - ay) * (px - ax)) / len;
                break;
            }
            case ConstraintKind::PointOnCircle:
                pt(q, ref(0), px, py), circ(q, ref(1), cx, cy, rr);
                r[k++] = std::hypot(px - cx, py - cy) - rr;
                break;
            case ConstraintKind::Midpoint:
                pt(q, ref(0), px, py), line(q, ref(1), ax, ay, bx, by);
                r[k++] = px - (ax + bx) / 2, r[k++] = py - (ay + by) / 2;
                break;
            case ConstraintKind::Symmetric: {
                pt(q, ref(0), px, py), pt(q, ref(1), qx, qy), line(q, ref(2), ax, ay, bx, by);
                double mx = (px + qx) / 2, my = (py + qy) / 2;
                double ux = bx - ax, uy = by - ay, len = std::max(std::hypot(ux, uy), 1e-12);
                r[k++] = (ux * (my - ay) - uy * (mx - ax)) / len;   // midpoint on the axis
                r[k++] = (ux * (qx - px) + uy * (qy - py)) / len;   // PQ perpendicular to the axis
                break;
            }
            case ConstraintKind::Concentric: {
                double c2x, c2y, r2;
                circ(q, ref(0), cx, cy, rr), circ(q, ref(1), c2x, c2y, r2);
                r[k++] = cx - c2x, r[k++] = cy - c2y;
                break;
            }
        }
    }

    const Sketch& s_;
    const Layout& L_;
    std::vector<ResolvedConstraint> cons_;
    std::vector<std::string> arcs_;
    std::vector<std::string> owner_;  // row -> constraint id
};

/// Nonlinear least squares for sketch constraints; returns max |residual|.
///
/// Primary step: minimum-norm Gauss-Newton (pseudo-inverse Newton),
///     dq = -J^T (J J^T + mu I)^{-1} f,
/// i.e. the smallest change of the geometry that satisfies the linearised
/// constraints J dq = -f. Sketches are usually under-determined (m < n); for
/// full row rank J this converges quadratically (Ben-Israel 1966, "A Newton-
/// Raphson method for the solution of systems of equations") and moves
/// unconstrained geometry as little as possible, which is what dragging needs.
/// mu (relative 1e-12 of the largest diagonal entry) only regularises
/// redundant rows. The step is globalised by backtracking on ||f||^2.
/// Fallback when the Newton direction does not decrease ||f|| (conflicting
/// constraints, far from a solution): Levenberg-Marquardt with Marquardt
/// scaling D = diag(J^T J).
double lm_solve(const System& sys, Vec& q, int* iterations = nullptr) {
    if (!sys.m()) return 0.0;
    Vec f = sys.residual(q);
    double lambda = 1e-3;
    int it = 0;
    for (; it < kMaxIterations && f.lpNorm<Eigen::Infinity>() > kSolveTolMm; ++it) {
        Eigen::MatrixXd J = sys.jacobian(q);
        const double f2 = f.squaredNorm();
        // --- minimum-norm Gauss-Newton with backtracking
        {
            Eigen::MatrixXd JJt = J * J.transpose();
            double mu = kNewtonMuRel * std::max(1.0, JJt.diagonal().maxCoeff());
            JJt.diagonal().array() += mu;
            Vec dq = -J.transpose() * JJt.ldlt().solve(f);
            bool accepted = false;
            for (double alpha = 1.0; alpha > kMinStep; alpha *= 0.5) {
                Vec qn = q + alpha * dq;
                Vec fn = sys.residual(qn);
                if (fn.squaredNorm() < f2 * (1 - kArmijo * alpha)) {
                    q = qn, f = fn;
                    accepted = true;
                    break;
                }
            }
            if (accepted) continue;
        }
        // --- Levenberg-Marquardt fallback
        Eigen::MatrixXd A = J.transpose() * J;
        Vec g = J.transpose() * f;
        Vec D = A.diagonal().cwiseMax(1e-12 * std::max(1.0, A.diagonal().maxCoeff()));
        bool improved = false;
        for (int tries = 0; tries < 20; ++tries) {
            Eigen::MatrixXd M = A;
            M.diagonal() += lambda * D;
            Vec dq = M.ldlt().solve(-g);
            Vec qn = q + dq;
            Vec fn = sys.residual(qn);
            if (fn.squaredNorm() < f2) {
                q = qn, f = fn;
                lambda = std::max(lambda / 3, 1e-12);
                improved = true;
                break;
            }
            lambda *= 4;
        }
        if (!improved) break;  // local minimum with nonzero residual: conflicting constraints
    }
    if (iterations) *iterations = it;
    return f.lpNorm<Eigen::Infinity>();
}

}  // namespace

std::pair<Sketch, SolveStatus> solve_sketch(const Sketch& s, const VariableResolver& vars, bool analyze) {
    SolveStatus st;
    Layout L = make_layout(s);
    // validate references and resolve values
    std::vector<ResolvedConstraint> cons;
    for (const auto& [id, c] : s.constraints) {
        for (const auto& r : c.refs)
            if (!s.points.count(r) && !s.curves.count(r))
                throw GeometryError("constraint " + id + " (" + constraint_name(c.kind) + ") refers to unknown '" + r + "'");
        ResolvedConstraint rc{id, c.kind, c.refs};
        if (constraint_has_value(c.kind)) {
            Dim d = c.kind == ConstraintKind::LineAngle ? dims::kAngle : dims::kLength;
            rc.v1 = resolve(c.value, vars, d, id.c_str());
            if (c.kind == ConstraintKind::Fix) rc.v2 = resolve(c.value2, vars, dims::kLength, id.c_str());
            if ((c.kind == ConstraintKind::LineLength || c.kind == ConstraintKind::Radius ||
                 c.kind == ConstraintKind::Diameter || c.kind == ConstraintKind::Distance) && !(rc.v1 > 0))
                throw GeometryError("constraint " + id + " (" + constraint_name(c.kind) + "): value must be > 0");
        }
        cons.push_back(std::move(rc));
    }
    for (const auto& [id, c] : s.curves) {
        std::size_t need = c.kind == CurveKind::Line ? 2 : c.kind == CurveKind::Circle ? 1 : 3;
        if (c.pts.size() != need) throw GeometryError("curve " + id + " needs " + std::to_string(need) + " point(s)");
        for (const auto& p : c.pts)
            if (!s.points.count(p)) throw GeometryError("curve " + id + " refers to unknown point '" + p + "'");
    }
    System sys(s, L, cons);
    Vec q(L.n);
    for (const auto& [id, p] : s.points) q[L.point_index[id]] = p.x, q[L.point_index[id] + 1] = p.y;
    for (const auto& [id, c] : s.curves)
        if (c.kind == CurveKind::Circle) q[L.radius_index[id]] = c.radius;
    st.variables = L.n;
    st.equations = sys.m();

    // --- Levenberg-Marquardt ---------------------------------------------------
    int it = 0;
    st.residual = lm_solve(sys, q, &it);
    Vec f = sys.m() ? sys.residual(q) : Vec(0);
    st.iterations = it;
    st.converged = st.residual <= kSolveTolMm * 10;

    // --- DOF analysis (SVD of J at the solution) ---------------------------------
    // BDCSVD (divide and conquer) falls back to one-sided Jacobi for small
    // matrices and is O(n^3) with a much smaller constant for large ones.
    if (analyze && L.n > 0) {
        Eigen::MatrixXd J = sys.m() ? sys.jacobian(q) : Eigen::MatrixXd::Zero(1, L.n);
        Eigen::BDCSVD<Eigen::MatrixXd> svd(J, Eigen::ComputeFullU | Eigen::ComputeFullV);
        const Vec& sv = svd.singularValues();
        double smax = sv.size() ? sv[0] : 0;
        int rank = 0;
        for (int i = 0; i < sv.size(); ++i)
            if (sv[i] > std::max(kRankRel * smax, 1e-12)) ++rank;
        if (!sys.m()) rank = 0;
        st.rank = rank;
        st.dof = L.n - rank;
        Eigen::MatrixXd Vn = svd.matrixV().rightCols(L.n - rank);
        auto var_free = [&](int i) { return Vn.cols() > 0 && Vn.row(i).norm() > kNullComponent; };
        for (const auto& [id, idx] : L.point_index)
            st.entity_state[id] = (var_free(idx) || var_free(idx + 1)) ? "free" : "fixed";
        for (const auto& [id, c] : s.curves) {
            bool free_ = false;
            for (const auto& p : c.pts) free_ = free_ || st.entity_state[p] == "free";
            if (c.kind == CurveKind::Circle) free_ = free_ || var_free(L.radius_index[id]);
            st.entity_state[id] = free_ ? "free" : "fixed";
        }
        for (const auto& [id, c] : s.constraints) st.constraint_state[id] = "ok";
        if (sys.m() > rank) {
            Eigen::MatrixXd Un = svd.matrixU().rightCols(sys.m() - rank);
            for (int row = 0; row < sys.m(); ++row)
                if (Un.row(row).norm() > kNullComponent) {
                    const std::string& id = sys.owner()[row];
                    if (id.rfind("arc:", 0) == 0) continue;
                    st.constraint_state[id] = st.converged ? "redundant" : "conflict";
                }
        }
    }
    // Conflicts: at a least-squares compromise the geometry is distorted and the
    // SVD shows no dependency, so find the constraints whose removal lets the
    // rest converge (leave-one-out; sketches are small).
    if (analyze && !st.converged && s.constraints.size() <= 200) {
        std::vector<std::string> culprits;
        for (const auto& [id, c] : s.constraints) {
            Sketch t = s;
            t.constraints.erase(id);
            Layout Lt = make_layout(t);
            std::vector<ResolvedConstraint> ct;
            for (const auto& rc : cons)
                if (rc.id != id) ct.push_back(rc);
            System st_sys(t, Lt, ct);
            Vec qt(Lt.n);
            for (const auto& [pid, p] : t.points) qt[Lt.point_index[pid]] = p.x, qt[Lt.point_index[pid] + 1] = p.y;
            for (const auto& [cid, cu] : t.curves)
                if (cu.kind == CurveKind::Circle) qt[Lt.radius_index[cid]] = cu.radius;
            if (lm_solve(st_sys, qt) <= kSolveTolMm * 10) culprits.push_back(id);
        }
        for (const auto& id : culprits) st.constraint_state[id] = "conflict";
        if (!culprits.empty()) {
            std::string list;
            for (const auto& id : culprits) list += (list.empty() ? "" : ", ") + id;
            st.message = "constraints conflict: removing one of " + list + " resolves it";
        }
    }
    std::ostringstream msg;
    if (!st.message.empty()) msg << st.message;
    else if (!st.converged) msg << "constraints conflict (residual " << st.residual << ")";
    else if (!analyze) msg << "solved";
    else if (st.dof == 0) msg << "fully constrained";
    else msg << st.dof << " degree(s) of freedom left";
    st.message = msg.str();

    Sketch out = s;
    for (auto& [id, p] : out.points) p.x = q[L.point_index[id]], p.y = q[L.point_index[id] + 1];
    for (auto& [id, c] : out.curves)
        if (c.kind == CurveKind::Circle) c.radius = q[L.radius_index[id]];
    return {out, st};
}

// ---------------------------------------------------------------- profiles
double ProfileSegment::sweep() const {
    if (!arc) return 0;
    if (full_circle) return ccw ? 2 * std::numbers::pi : -2 * std::numbers::pi;
    double a0 = std::atan2(ay - cy, ax - cx), a1 = std::atan2(by - cy, bx - cx);
    double ccw_sweep = std::fmod(a1 - a0 + 4 * std::numbers::pi, 2 * std::numbers::pi);
    return ccw ? ccw_sweep : -(2 * std::numbers::pi - ccw_sweep);
}

void ProfileSegment::at(double t, double& x, double& y) const {
    if (!arc) {
        x = ax + (bx - ax) * t, y = ay + (by - ay) * t;
        return;
    }
    double a = std::atan2(ay - cy, ax - cx) + sweep() * t;
    x = cx + r * std::cos(a), y = cy + r * std::sin(a);
}

namespace {
double seg_area(const ProfileSegment& g) {
    // contribution to the closed-curve integral (1/2) \oint (x dy - y dx);
    // for an arc with signed sweep phi: (r^2 phi + cx (by - ay) - cy (bx - ax)) / 2
    if (!g.arc) return (g.ax * g.by - g.bx * g.ay) / 2;
    return (g.r * g.r * g.sweep() + g.cx * (g.by - g.ay) - g.cy * (g.bx - g.ax)) / 2;
}
std::vector<std::pair<double, double>> sample(const ProfileLoop& l) {
    std::vector<std::pair<double, double>> pts;
    for (const auto& g : l.segs) {
        int n = g.arc ? 32 : 1;
        for (int i = 0; i < n; ++i) {
            double x, y;
            g.at(double(i) / n, x, y);
            pts.push_back({x, y});
        }
    }
    return pts;
}
bool inside(const std::vector<std::pair<double, double>>& poly, double x, double y) {
    bool in = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        auto [xi, yi] = poly[i];
        auto [xj, yj] = poly[j];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
    }
    return in;
}
}  // namespace

std::vector<ProfileFace> sketch_profiles(const Sketch& s) {
    // vertices: cluster endpoints by position
    struct End { std::string curve; int which; double x, y; };
    std::vector<ProfileLoop> loops;
    std::vector<std::string> open_curves;
    std::vector<End> ends;
    for (const auto& [id, c] : s.curves) {
        if (c.construction) continue;
        if (c.kind == CurveKind::Circle) {
            const auto& p = s.points.at(c.pts[0]);
            ProfileSegment g;
            g.arc = true, g.full_circle = true;
            g.ax = g.bx = p.x + c.radius, g.ay = g.by = p.y;
            g.cx = p.x, g.cy = p.y, g.r = c.radius;
            ProfileLoop l;
            l.segs.push_back(g);
            l.area = std::numbers::pi * c.radius * c.radius;
            loops.push_back(l);
            continue;
        }
        const auto& a = s.points.at(c.kind == CurveKind::Line ? c.pts[0] : c.pts[1]);
        const auto& b = s.points.at(c.kind == CurveKind::Line ? c.pts[1] : c.pts[2]);
        ends.push_back({id, 0, a.x, a.y});
        ends.push_back({id, 1, b.x, b.y});
    }
    // vertex id per end
    std::vector<int> vid(ends.size(), -1);
    std::vector<std::pair<double, double>> verts;
    for (std::size_t i = 0; i < ends.size(); ++i) {
        for (std::size_t v = 0; v < verts.size(); ++v)
            if (std::hypot(ends[i].x - verts[v].first, ends[i].y - verts[v].second) < kLoopJoinMm) {
                vid[i] = int(v);
                break;
            }
        if (vid[i] < 0) vid[i] = int(verts.size()), verts.push_back({ends[i].x, ends[i].y});
    }
    std::vector<std::vector<int>> at(verts.size());  // vertex -> end indices
    for (std::size_t i = 0; i < ends.size(); ++i) at[vid[i]].push_back(int(i));
    for (std::size_t v = 0; v < verts.size(); ++v)
        if (at[v].size() != 2) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "profile is %s at (%.4g, %.4g): %zu curve end(s) meet there, a closed profile needs exactly 2",
                          at[v].size() < 2 ? "open" : "branching", verts[v].first, verts[v].second, at[v].size());
            throw GeometryError(buf);
        }
    std::vector<char> used(ends.size() / 2, 0);
    auto curve_of = [&](int end) { return end / 2; };
    for (std::size_t start = 0; start < used.size(); ++start) {
        if (used[start]) continue;
        ProfileLoop loop;
        int e = int(2 * start);  // start at end 0 of this curve, walk to end 1
        for (;;) {
            int ci = curve_of(e);
            if (used[ci]) break;
            used[ci] = 1;
            const auto& ed = ends[e];
            const auto& cu = s.curves.at(ed.curve);
            int other = e ^ 1;
            ProfileSegment g{};
            g.ax = ends[e].x, g.ay = ends[e].y, g.bx = ends[other].x, g.by = ends[other].y;
            if (cu.kind == CurveKind::Arc) {
                const auto& c = s.points.at(cu.pts[0]);
                g.arc = true, g.cx = c.x, g.cy = c.y;
                g.r = std::hypot(g.ax - c.x, g.ay - c.y);
                g.ccw = ed.which == 0;  // arcs are defined counter-clockwise start -> end
            }
            loop.segs.push_back(g);
            int v = vid[other];
            int next = at[v][0] == other ? at[v][1] : at[v][0];
            e = next;
            if (curve_of(e) == int(start)) break;
        }
        double area = 0;
        for (const auto& g : loop.segs) area += seg_area(g);
        loop.area = area;
        loops.push_back(loop);
    }
    // nesting: a loop is a hole of the smallest loop containing it
    std::vector<std::vector<std::pair<double, double>>> poly;
    for (const auto& l : loops) poly.push_back(sample(l));
    std::vector<int> parent(loops.size(), -1);
    for (std::size_t i = 0; i < loops.size(); ++i) {
        auto [x, y] = poly[i].front();
        double best = INFINITY;
        for (std::size_t j = 0; j < loops.size(); ++j)
            if (i != j && std::abs(loops[j].area) > std::abs(loops[i].area) && inside(poly[j], x, y) &&
                std::abs(loops[j].area) < best)
                best = std::abs(loops[j].area), parent[i] = int(j);
    }
    std::vector<ProfileFace> faces;
    std::map<int, std::size_t> face_of;
    for (std::size_t i = 0; i < loops.size(); ++i) {
        int depth = 0;
        for (int p = parent[i]; p >= 0; p = parent[p]) ++depth;
        if (depth % 2 == 0) face_of[int(i)] = faces.size(), faces.push_back({loops[i], {}});
    }
    for (std::size_t i = 0; i < loops.size(); ++i) {
        int depth = 0;
        for (int p = parent[i]; p >= 0; p = parent[p]) ++depth;
        if (depth % 2 == 1) faces[face_of[parent[i]]].holes.push_back(loops[i]);
    }
    if (faces.empty()) throw GeometryError("sketch has no closed profile");
    return faces;
}

std::vector<ProfileSegment> sketch_path(const Sketch& s) {
    struct Seg { ProfileSegment g; std::string a, b; };
    std::vector<Seg> segs;
    for (const auto& [id, c] : s.curves) {
        if (c.construction) continue;
        if (c.kind == CurveKind::Circle) throw GeometryError("a sweep path cannot contain full circles");
        ProfileSegment g;
        std::string pa = c.kind == CurveKind::Line ? c.pts[0] : c.pts[1], pb = c.kind == CurveKind::Line ? c.pts[1] : c.pts[2];
        g.ax = s.points.at(pa).x, g.ay = s.points.at(pa).y, g.bx = s.points.at(pb).x, g.by = s.points.at(pb).y;
        if (c.kind == CurveKind::Arc) {
            g.arc = true, g.cx = s.points.at(c.pts[0]).x, g.cy = s.points.at(c.pts[0]).y;
            g.r = std::hypot(g.ax - g.cx, g.ay - g.cy);
        }
        segs.push_back({g, pa, pb});
    }
    if (segs.empty()) throw GeometryError("sweep path sketch is empty");
    auto same = [&](double x0, double y0, double x1, double y1) { return std::hypot(x0 - x1, y0 - y1) < kLoopJoinMm; };
    // find an end that touches no other segment
    auto degree = [&](double x, double y) {
        int d = 0;
        for (const auto& t : segs) d += same(t.g.ax, t.g.ay, x, y) + same(t.g.bx, t.g.by, x, y);
        return d;
    };
    std::size_t start = segs.size();
    bool flip = false;
    for (std::size_t i = 0; i < segs.size() && start == segs.size(); ++i) {
        if (degree(segs[i].g.ax, segs[i].g.ay) == 1) start = i, flip = false;
        else if (degree(segs[i].g.bx, segs[i].g.by) == 1) start = i, flip = true;
    }
    if (start == segs.size()) throw GeometryError("sweep path must be an open chain (it is closed)");
    std::vector<ProfileSegment> out;
    std::vector<char> used(segs.size(), 0);
    std::size_t cur = start;
    for (;;) {
        ProfileSegment g = segs[cur].g;
        if (flip) std::swap(g.ax, g.bx), std::swap(g.ay, g.by), g.ccw = !g.ccw;
        out.push_back(g);
        used[cur] = 1;
        std::size_t next = segs.size();
        for (std::size_t j = 0; j < segs.size(); ++j) {
            if (used[j]) continue;
            if (same(segs[j].g.ax, segs[j].g.ay, g.bx, g.by)) next = j, flip = false;
            else if (same(segs[j].g.bx, segs[j].g.by, g.bx, g.by)) next = j, flip = true;
            if (next != segs.size()) break;
        }
        if (next == segs.size()) break;
        cur = next;
    }
    if (out.size() != segs.size()) throw GeometryError("sweep path must be one connected chain");
    return out;
}

std::vector<SketchChain> sketch_chains(const Sketch& s, double chord) {
    struct Seg { std::vector<std::pair<double, double>> pts; bool used = false; };
    std::vector<Seg> segs;
    std::vector<SketchChain> out;
    for (const auto& [id, c] : s.curves) {
        if (c.construction) continue;
        const auto& p0 = s.points.at(c.pts[0]);
        if (c.kind == CurveKind::Line) {
            const auto& p1 = s.points.at(c.pts[1]);
            segs.push_back({{{p0.x, p0.y}, {p1.x, p1.y}}});
            continue;
        }
        double r = c.radius, a0 = 0, sweep = 2 * std::numbers::pi;
        if (c.kind == CurveKind::Arc) {
            const auto &st = s.points.at(c.pts[1]), &en = s.points.at(c.pts[2]);
            r = std::hypot(st.x - p0.x, st.y - p0.y);
            a0 = std::atan2(st.y - p0.y, st.x - p0.x);
            sweep = std::fmod(std::atan2(en.y - p0.y, en.x - p0.x) - a0 + 4 * std::numbers::pi, 2 * std::numbers::pi);
        }
        if (!(r > 0)) continue;
        int n = std::max(3, int(std::ceil(circle_segments(r, chord) * sweep / (2 * std::numbers::pi))));
        Seg g;
        for (int i = 0; i <= n; ++i) {
            if (c.kind == CurveKind::Circle && i == n) break;
            double t = a0 + sweep * i / n;
            g.pts.push_back({p0.x + r * std::cos(t), p0.y + r * std::sin(t)});
        }
        if (c.kind == CurveKind::Circle) out.push_back({g.pts, true});
        else segs.push_back(g);
    }
    auto same = [](std::pair<double, double> a, std::pair<double, double> b) {
        return std::hypot(a.first - b.first, a.second - b.second) < kLoopJoinMm;
    };
    for (std::size_t i = 0; i < segs.size(); ++i) {
        if (segs[i].used) continue;
        segs[i].used = true;
        std::vector<std::pair<double, double>> chain = segs[i].pts;
        bool grew = true;
        while (grew) {  // extend at both ends
            grew = false;
            for (auto& t : segs) {
                if (t.used) continue;
                if (same(chain.back(), t.pts.front())) chain.insert(chain.end(), t.pts.begin() + 1, t.pts.end());
                else if (same(chain.back(), t.pts.back())) chain.insert(chain.end(), t.pts.rbegin() + 1, t.pts.rend());
                else if (same(chain.front(), t.pts.back())) chain.insert(chain.begin(), t.pts.begin(), t.pts.end() - 1);
                else if (same(chain.front(), t.pts.front())) chain.insert(chain.begin(), t.pts.rbegin(), t.pts.rend() - 1);
                else continue;
                t.used = true;
                grew = true;
            }
        }
        bool closed = chain.size() > 2 && same(chain.front(), chain.back());
        if (closed) chain.pop_back();
        out.push_back({chain, closed});
    }
    return out;
}

Mesh sketch_lines(const Sketch& s, double chord, bool construction) {
    Mesh m;
    auto add = [&](double x0, double y0, double x1, double y1) {
        m.V.push_back(s.to_world(x0, y0));
        m.V.push_back(s.to_world(x1, y1));
    };
    for (const auto& [id, c] : s.curves) {
        if (c.construction && !construction) continue;
        if (c.kind == CurveKind::Line) {
            const auto &a = s.points.at(c.pts[0]), &b = s.points.at(c.pts[1]);
            add(a.x, a.y, b.x, b.y);
            continue;
        }
        const auto& ce = s.points.at(c.pts[0]);
        double r = c.radius, a0 = 0, sweep = 2 * std::numbers::pi;
        if (c.kind == CurveKind::Arc) {
            const auto &st = s.points.at(c.pts[1]), &en = s.points.at(c.pts[2]);
            r = std::hypot(st.x - ce.x, st.y - ce.y);
            a0 = std::atan2(st.y - ce.y, st.x - ce.x);
            double a1 = std::atan2(en.y - ce.y, en.x - ce.x);
            sweep = std::fmod(a1 - a0 + 4 * std::numbers::pi, 2 * std::numbers::pi);
        }
        if (!(r > 0)) continue;
        int n = std::max(4, int(std::ceil(circle_segments(r, chord) * sweep / (2 * std::numbers::pi))));
        for (int i = 0; i < n; ++i) {
            double t0 = a0 + sweep * i / n, t1 = a0 + sweep * (i + 1) / n;
            add(ce.x + r * std::cos(t0), ce.y + r * std::sin(t0), ce.x + r * std::cos(t1), ce.y + r * std::sin(t1));
        }
    }
    return m;
}

std::string sketch_to_python(const Sketch& s) {
    std::ostringstream o;
    o << "Sketch(\"" << plane_name(s.plane) << "\"";
    if (s.offset != 0) o << ", offset=" << repr_double(s.offset) << "*mm";
    o << ", points={";
    bool first = true;
    for (const auto& [id, p] : s.points) {
        o << (first ? "" : ", ") << "\"" << id << "\": (" << repr_double(p.x) << ", " << repr_double(p.y) << ")";
        first = false;
    }
    o << "}, curves={";
    first = true;
    for (const auto& [id, c] : s.curves) {
        o << (first ? "" : ", ") << "\"" << id << "\": (\""
          << (c.kind == CurveKind::Line ? "line" : c.kind == CurveKind::Circle ? "circle" : "arc") << "\"";
        for (const auto& p : c.pts) o << ", \"" << p << "\"";
        if (c.kind == CurveKind::Circle) o << ", " << repr_double(c.radius);
        if (c.construction) o << ", \"construction\"";
        o << ")";
        first = false;
    }
    o << "}, constraints={";
    first = true;
    for (const auto& [id, c] : s.constraints) {
        o << (first ? "" : ", ") << "\"" << id << "\": (\"" << constraint_name(c.kind) << "\", [";
        for (std::size_t i = 0; i < c.refs.size(); ++i) o << (i ? ", " : "") << "\"" << c.refs[i] << "\"";
        o << "]";
        if (constraint_has_value(c.kind)) {
            o << ", " << param_to_python(c.value, c.kind == ConstraintKind::LineAngle ? dims::kAngle : dims::kLength);
            if (c.kind == ConstraintKind::Fix) o << ", " << param_to_python(c.value2, dims::kLength);
        }
        o << ")";
        first = false;
    }
    o << "})";
    return o.str();
}

}  // namespace anvil
