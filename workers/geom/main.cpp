// anvil-geom: the exact B-Rep kernel (OpenCASCADE) in its own process (ADR 0001).
//
// Reads framed JSON requests on stdin, writes framed responses on stdout
// (protocol: core/include/anvil/geom_client.hpp). Any crash here only ends
// this process; the application restarts it and reports the failing feature.
#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepOffsetAPI_MakePipe.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <numbers>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

using nlohmann::json;

namespace {

struct KernelError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------- framing
bool read_exact(void* dst, std::size_t n) {
    return std::fread(dst, 1, n, stdin) == n;
}
bool read_frame(std::string& out) {
    unsigned char len[4];
    if (!read_exact(len, 4)) return false;
    std::uint32_t n = len[0] | len[1] << 8 | len[2] << 16 | std::uint32_t(len[3]) << 24;
    out.resize(n);
    return n == 0 || read_exact(out.data(), n);
}
// Protocol output: a private duplicate of the original stdout. stdout itself is
// redirected to stderr, so library chatter (OCCT prints to std::cout) can never
// corrupt the binary protocol.
std::FILE* g_proto = nullptr;

void write_frame(const std::string& s) {
    std::uint32_t n = std::uint32_t(s.size());
    unsigned char len[4] = {std::uint8_t(n), std::uint8_t(n >> 8), std::uint8_t(n >> 16), std::uint8_t(n >> 24)};
    std::fwrite(len, 1, 4, g_proto);
    std::fwrite(s.data(), 1, s.size(), g_proto);
    std::fflush(g_proto);
}

// ---------------------------------------------------------------- helpers
double P(const json& n, const char* k) { return n.at("p").at(k).get<double>(); }
std::string A(const json& n, const char* k, const std::string& def = "") {
    if (!n.contains("a") || !n["a"].contains(k)) return def;
    return n["a"][k].get<std::string>();
}
gp_Pnt pnt(const json& v) { return gp_Pnt(v[0].get<double>(), v[1].get<double>(), v[2].get<double>()); }
gp_Vec vec(const json& v) { return gp_Vec(v[0].get<double>(), v[1].get<double>(), v[2].get<double>()); }

struct Frame {
    gp_Pnt o;
    gp_Vec u, v, n;
    gp_Pnt at(double x, double y) const { return o.Translated(u * x + v * y); }
};
Frame frame_of(const json& sk) { return {pnt(sk["origin"]), vec(sk["u"]), vec(sk["v"]), vec(sk["n"])}; }

TopoDS_Edge seg_edge(const Frame& f, const json& g) {
    double ax = g["a"][0], ay = g["a"][1], bx = g["b"][0], by = g["b"][1];
    if (!g["arc"].get<bool>()) return BRepBuilderAPI_MakeEdge(f.at(ax, ay), f.at(bx, by));
    double cx = g["c"][0], cy = g["c"][1], r = g["r"];
    if (g["full"].get<bool>()) {
        gp_Ax2 ax2(f.at(cx, cy), gp_Dir(f.n), gp_Dir(f.u));
        return BRepBuilderAPI_MakeEdge(gp_Circ(ax2, r));
    }
    double a0 = std::atan2(ay - cy, ax - cx), a1 = std::atan2(by - cy, bx - cx);
    double ccw_sweep = std::fmod(a1 - a0 + 4 * std::numbers::pi, 2 * std::numbers::pi);
    double sweep = g["ccw"].get<bool>() ? ccw_sweep : -(2 * std::numbers::pi - ccw_sweep);
    double am = a0 + sweep / 2;
    Handle(Geom_TrimmedCurve) arc = GC_MakeArcOfCircle(f.at(ax, ay), f.at(cx + r * std::cos(am), cy + r * std::sin(am)), f.at(bx, by));
    return BRepBuilderAPI_MakeEdge(arc);
}

TopoDS_Wire loop_wire(const Frame& f, const json& segs) {
    BRepBuilderAPI_MakeWire w;
    for (const auto& g : segs) w.Add(seg_edge(f, g));
    if (!w.IsDone()) throw KernelError("sketch profile could not be turned into a closed wire");
    return w.Wire();
}

std::vector<TopoDS_Face> sketch_faces(const json& sk) {
    if (!sk.contains("faces")) throw KernelError(sk.value("profile_error", std::string("sketch has no closed profile")));
    Frame f = frame_of(sk);
    std::vector<TopoDS_Face> out;
    for (const auto& fj : sk["faces"]) {
        BRepBuilderAPI_MakeFace mf(loop_wire(f, fj["outer"]), true);
        if (!mf.IsDone()) throw KernelError("sketch profile is not planar or self-intersects");
        TopoDS_Face face = mf.Face();
        for (const auto& h : fj["holes"]) {
            TopoDS_Wire hw = loop_wire(f, h);
            BRepBuilderAPI_MakeFace with_hole(face);
            with_hole.Add(TopoDS::Wire(hw.Reversed()));
            face = with_hole.Face();
        }
        out.push_back(face);
    }
    return out;
}

TopoDS_Shape fuse_all(const std::vector<TopoDS_Shape>& parts) {
    if (parts.empty()) throw KernelError("nothing to combine");
    TopoDS_Shape acc = parts[0];
    for (std::size_t i = 1; i < parts.size(); ++i) {
        BRepAlgoAPI_Fuse fu(acc, parts[i]);
        if (!fu.IsDone() || fu.HasErrors()) throw KernelError("union failed");
        acc = fu.Shape();
    }
    return acc;
}

TopoDS_Shape transformed(const TopoDS_Shape& s, const gp_Trsf& t) { return BRepBuilderAPI_Transform(s, t, true).Shape(); }

// Edge selection by geometry (stable under dimension changes):
//   all | top | bottom | vertical | horizontal | near:x,y,z;x,y,z
std::vector<TopoDS_Edge> select_edges(const TopoDS_Shape& s, const std::string& sel) {
    Bnd_Box box;
    BRepBndLib::Add(s, box);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    const double tol = 1e-6 * std::max(1.0, std::max({x1 - x0, y1 - y0, z1 - z0}));
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(s, TopAbs_EDGE, edges);
    std::vector<TopoDS_Edge> out;
    if (sel.rfind("near:", 0) == 0) {
        std::string rest = sel.substr(5);
        std::size_t pos = 0;
        while (pos < rest.size()) {
            std::size_t e = rest.find(';', pos);
            std::string item = rest.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            pos = e == std::string::npos ? rest.size() : e + 1;
            double x, y, z;
            if (std::sscanf(item.c_str(), "%lf,%lf,%lf", &x, &y, &z) != 3) throw KernelError("bad edge selector '" + item + "'");
            TopoDS_Vertex v = BRepBuilderAPI_MakeVertex(gp_Pnt(x, y, z));
            double best = 1e300;
            int bi = -1;
            for (int i = 1; i <= edges.Extent(); ++i) {
                BRepExtrema_DistShapeShape d(v, edges(i));
                if (d.IsDone() && d.Value() < best) best = d.Value(), bi = i;
            }
            if (bi > 0) out.push_back(TopoDS::Edge(edges(bi)));
        }
        return out;
    }
    for (int i = 1; i <= edges.Extent(); ++i) {
        const TopoDS_Edge& e = TopoDS::Edge(edges(i));
        if (BRep_Tool::Degenerated(e)) continue;
        // sample the curve itself (bounding boxes include tolerance padding)
        BRepAdaptor_Curve c(e);
        double t0 = c.FirstParameter(), t1 = c.LastParameter();
        double zmin = 1e300, zmax = -1e300, xmin = 1e300, xmax = -1e300, ymin = 1e300, ymax = -1e300;
        for (int k = 0; k <= 4; ++k) {
            gp_Pnt q = c.Value(t0 + (t1 - t0) * k / 4);
            zmin = std::min(zmin, q.Z()), zmax = std::max(zmax, q.Z());
            xmin = std::min(xmin, q.X()), xmax = std::max(xmax, q.X());
            ymin = std::min(ymin, q.Y()), ymax = std::max(ymax, q.Y());
        }
        bool flat = (zmax - zmin) < tol;
        bool vert = (xmax - xmin) < tol && (ymax - ymin) < tol && (zmax - zmin) > tol;
        bool pick = sel == "all" || (sel == "top" && flat && std::abs(zmax - z1) < tol) ||
                    (sel == "bottom" && flat && std::abs(zmin - z0) < tol) || (sel == "vertical" && vert) ||
                    (sel == "horizontal" && flat);
        if (pick) out.push_back(e);
    }
    if (out.empty()) throw KernelError("edge selector '" + sel + "' matches no edge");
    return out;
}

// ---------------------------------------------------------------- builder
struct Builder {
    std::unordered_map<std::string, TopoDS_Shape> cache;  // subtree JSON -> shape

    TopoDS_Shape build(const json& n) {
        std::string key = n.dump();
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        TopoDS_Shape s = make(n);
        if (cache.size() > 256) cache.clear();
        cache[key] = s;
        return s;
    }

    TopoDS_Shape make(const json& n) {
        const std::string k = n.at("k");
        const json& in = n.at("in");
        if (k == "box") {
            double x = P(n, "x"), y = P(n, "y"), z = P(n, "z");
            return BRepPrimAPI_MakeBox(gp_Pnt(-x / 2, -y / 2, 0), x, y, z).Shape();
        }
        if (k == "cylinder") {  // revolved profile, identical definition to the mesh kernel
            double h = P(n, "h"), r0 = P(n, "d") / 2, r1 = P(n, "d2") / 2;
            double ct = P(n, "chamfer_top"), cb = P(n, "chamfer_bottom");
            auto rside = [&](double z) { return r0 + (r1 - r0) * z / h; };
            BRepBuilderAPI_MakePolygon poly;
            poly.Add(gp_Pnt(0, 0, 0));
            if (cb > 0) poly.Add(gp_Pnt(r0 - cb, 0, 0)), poly.Add(gp_Pnt(rside(cb), 0, cb));
            else poly.Add(gp_Pnt(r0, 0, 0));
            if (ct > 0) poly.Add(gp_Pnt(rside(h - ct), 0, h - ct)), poly.Add(gp_Pnt(r1 - ct, 0, h));
            else if (r1 > 0) poly.Add(gp_Pnt(r1, 0, h));
            poly.Add(gp_Pnt(0, 0, h));
            poly.Close();
            TopoDS_Face face = BRepBuilderAPI_MakeFace(poly.Wire(), true);
            return BRepPrimAPI_MakeRevol(face, gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1))).Shape();
        }
        if (k == "sphere") return BRepPrimAPI_MakeSphere(P(n, "d") / 2).Shape();
        if (k == "torus") return BRepPrimAPI_MakeTorus(P(n, "R"), P(n, "r")).Shape();
        if (k == "boolean") {
            TopoDS_Shape a = build(in[0]), b = build(in[1]);
            std::string op = n.at("op");
            TopoDS_Shape r;
            bool ok;
            if (op == "union") {
                BRepAlgoAPI_Fuse x(a, b);
                ok = x.IsDone() && !x.HasErrors(), r = ok ? x.Shape() : r;
            } else if (op == "difference") {
                BRepAlgoAPI_Cut x(a, b);
                ok = x.IsDone() && !x.HasErrors(), r = ok ? x.Shape() : r;
            } else {
                BRepAlgoAPI_Common x(a, b);
                ok = x.IsDone() && !x.HasErrors(), r = ok ? x.Shape() : r;
            }
            if (!ok) throw KernelError("boolean " + op + " failed in the B-Rep kernel");
            return r;
        }
        if (k == "transform") {
            gp_Trsf rx, ry, rz, t;
            rx.SetRotation(gp_Ax1(gp_Pnt(), gp_Dir(1, 0, 0)), P(n, "rx"));
            ry.SetRotation(gp_Ax1(gp_Pnt(), gp_Dir(0, 1, 0)), P(n, "ry"));
            rz.SetRotation(gp_Ax1(gp_Pnt(), gp_Dir(0, 0, 1)), P(n, "rz"));
            t.SetTranslation(gp_Vec(P(n, "tx"), P(n, "ty"), P(n, "tz")));
            gp_Trsf all = t * rz * ry * rx;  // rotate X, then Y, then Z, then translate
            return transformed(build(in[0]), all);
        }
        if (k == "extrude") {
            const json& sk = in[0];
            if (sk.at("k") != "sketch") throw KernelError("extrude needs a sketch");
            Frame f = frame_of(sk);
            double d = P(n, "distance");
            bool sym = P(n, "symmetric") > 0.5;
            std::vector<TopoDS_Shape> parts;
            for (auto& face : sketch_faces(sk)) {
                TopoDS_Shape base = face;
                if (sym) {
                    gp_Trsf t;
                    t.SetTranslation(f.n * (-d / 2));
                    base = transformed(face, t);
                }
                parts.push_back(BRepPrimAPI_MakePrism(base, f.n * d).Shape());
            }
            return fuse_all(parts);
        }
        if (k == "revolve") {
            const json& sk = in[0];
            Frame f = frame_of(sk);
            gp_Dir axis = P(n, "axis") > 0.5 ? gp_Dir(f.v) : gp_Dir(f.u);
            double ang = P(n, "angle");
            std::vector<TopoDS_Shape> parts;
            for (auto& face : sketch_faces(sk)) {
                BRepPrimAPI_MakeRevol rv(face, gp_Ax1(f.o, axis), ang);
                if (!rv.IsDone()) throw KernelError("revolve failed (does the profile cross the axis?)");
                parts.push_back(rv.Shape());
            }
            return fuse_all(parts);
        }
        if (k == "fillet" || k == "chamfer") {
            TopoDS_Shape s = build(in[0]);
            auto edges = select_edges(s, A(n, "edges", "all"));
            TopoDS_Shape r;
            if (k == "fillet") {
                BRepFilletAPI_MakeFillet mk(s);
                for (auto& e : edges) mk.Add(P(n, "r"), e);
                mk.Build();
                if (!mk.IsDone()) throw KernelError("fillet r=" + std::to_string(P(n, "r")) + " mm failed (radius too large for the selected edges?)");
                r = mk.Shape();
            } else {
                BRepFilletAPI_MakeChamfer mk(s);
                for (auto& e : edges) mk.Add(P(n, "d"), e);
                mk.Build();
                if (!mk.IsDone()) throw KernelError("chamfer d=" + std::to_string(P(n, "d")) + " mm failed (distance too large?)");
                r = mk.Shape();
            }
            return r;
        }
        if (k == "shell") {
            TopoDS_Shape s = build(in[0]);
            std::string which = A(n, "faces", "top");
            Bnd_Box box;
            BRepBndLib::Add(s, box);
            double x0, y0, z0, x1, y1, z1;
            box.Get(x0, y0, z0, x1, y1, z1);
            TopTools_ListOfShape remove;
            for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
                Bnd_Box fb;
                BRepBndLib::Add(ex.Current(), fb);
                double a0, b0, c0, a1, b1, c1;
                fb.Get(a0, b0, c0, a1, b1, c1);
                double tol = 1e-6 * std::max(1.0, z1 - z0);
                if ((which == "top" && std::abs(c0 - z1) < tol && std::abs(c1 - z1) < tol) ||
                    (which == "bottom" && std::abs(c0 - z0) < tol && std::abs(c1 - z0) < tol))
                    remove.Append(ex.Current());
            }
            BRepOffsetAPI_MakeThickSolid mk;
            mk.MakeThickSolidByJoin(s, remove, -P(n, "t"), 1e-6);
            if (!mk.IsDone()) throw KernelError("shell t=" + std::to_string(P(n, "t")) + " mm failed (wall thicker than the part?)");
            return mk.Shape();
        }
        if (k == "linearpattern" || k == "circularpattern") {
            TopoDS_Shape s = build(in[0]);
            int count = int(std::lround(P(n, "n")));
            std::vector<TopoDS_Shape> parts;
            for (int i = 0; i < count; ++i) {
                gp_Trsf t;
                if (k == "linearpattern") t.SetTranslation(gp_Vec(P(n, "dx") * i, P(n, "dy") * i, P(n, "dz") * i));
                else {
                    double total = P(n, "angle");
                    bool full = std::abs(total - 2 * std::numbers::pi) < 1e-12;
                    t.SetRotation(gp_Ax1(gp_Pnt(), gp_Dir(0, 0, 1)), total / (full ? count : std::max(count - 1, 1)) * i);
                }
                parts.push_back(transformed(s, t));
            }
            return fuse_all(parts);
        }
        if (k == "loft") {
            BRepOffsetAPI_ThruSections th(true, P(n, "ruled") > 0.5);
            for (const auto& sk : in) {
                Frame f = frame_of(sk);
                if (!sk.contains("faces")) throw KernelError("loft section: " + sk.value("profile_error", std::string()));
                th.AddWire(loop_wire(f, sk["faces"][0]["outer"]));
            }
            th.Build();
            if (!th.IsDone()) throw KernelError("loft failed (sections must be closed and compatible)");
            return th.Shape();
        }
        if (k == "sweep") {
            const json& prof = in[0];
            const json& path = in[1];
            if (!path.contains("path")) throw KernelError("sweep path: " + path.value("path_error", std::string()));
            Frame pf = frame_of(path);
            TopoDS_Wire spine = loop_wire(pf, path["path"]);
            std::vector<TopoDS_Shape> parts;
            for (auto& face : sketch_faces(prof)) {
                BRepOffsetAPI_MakePipe pipe(spine, face);
                pipe.Build();
                if (!pipe.IsDone()) throw KernelError("sweep failed (profile must sit at the path start)");
                parts.push_back(pipe.Shape());
            }
            return fuse_all(parts);
        }
        if (k == "step") {
            STEPControl_Reader rd;
            std::string path = A(n, "path");
            if (rd.ReadFile(path.c_str()) != IFSelect_RetDone) throw KernelError("cannot read STEP file '" + path + "'");
            rd.TransferRoots();
            TopoDS_Shape s = rd.OneShape();
            if (s.IsNull()) throw KernelError("STEP file '" + path + "' contains no shape");
            return s;
        }
        if (k == "sketch") throw KernelError("a sketch is not a solid (extrude or revolve it)");
        throw KernelError("unsupported feature '" + k + "' in the B-Rep kernel");
    }
};

// Tessellate and weld (faces share edge discretisations, so equal positions are bit-identical).
void mesh_shape(const TopoDS_Shape& s, double chord, std::vector<double>& V, std::vector<std::uint32_t>& F) {
    BRepMesh_IncrementalMesh mesher(s, chord, false, 0.5, false);
    struct Key {
        double x, y, z;
        bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct H {
        std::size_t operator()(const Key& k) const {
            std::uint64_t a, b, c;
            std::memcpy(&a, &k.x, 8), std::memcpy(&b, &k.y, 8), std::memcpy(&c, &k.z, 8);
            return std::size_t(a * 0x9E3779B97F4A7C15ull ^ b * 0xC2B2AE3D27D4EB4Full ^ c * 0x165667B19E3779F9ull);
        }
    };
    std::unordered_map<Key, std::uint32_t, H> idx;
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
        const TopoDS_Face& face = TopoDS::Face(ex.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull()) throw KernelError("a face could not be tessellated");
        gp_Trsf tr = loc.Transformation();
        std::vector<std::uint32_t> map(std::size_t(tri->NbNodes()) + 1);
        for (int i = 1; i <= tri->NbNodes(); ++i) {
            gp_Pnt p = tri->Node(i).Transformed(tr);
            Key key{p.X(), p.Y(), p.Z()};
            auto [it, ins] = idx.try_emplace(key, std::uint32_t(V.size() / 3));
            if (ins) V.insert(V.end(), {p.X(), p.Y(), p.Z()});
            map[i] = it->second;
        }
        bool rev = face.Orientation() == TopAbs_REVERSED;
        for (int i = 1; i <= tri->NbTriangles(); ++i) {
            int a, b, c;
            tri->Triangle(i).Get(a, b, c);
            std::uint32_t A_ = map[a], B_ = map[b], C_ = map[c];
            if (A_ == B_ || B_ == C_ || A_ == C_) continue;  // collapsed by welding
            if (rev) std::swap(B_, C_);
            F.insert(F.end(), {A_, B_, C_});
        }
    }
}

}  // namespace

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    int proto_fd = _dup(_fileno(stdout));
    std::FILE* devnull = std::fopen("NUL", "w");  // OCCT status chatter goes nowhere
    _dup2(_fileno(devnull), _fileno(stdout));
    g_proto = _fdopen(proto_fd, "wb");
#else
    int proto_fd = dup(fileno(stdout));
    std::FILE* devnull = std::fopen("/dev/null", "w");  // OCCT status chatter goes nowhere
    dup2(fileno(devnull), fileno(stdout));
    g_proto = fdopen(proto_fd, "wb");
#endif
    Builder builder;
    std::string req;
    while (read_frame(req)) {
        json reply;
        std::string payload;
        try {
            json r = json::parse(req);
            const std::string op = r.at("op");
            if (op == "ping") {
                reply = {{"ok", true}};
            } else if (op == "crash") {
                std::abort();  // test hook: crash isolation
            } else if (op == "eval") {
                TopoDS_Shape s = builder.build(r.at("tree"));
                BRepCheck_Analyzer check(s);
                if (!check.IsValid()) throw KernelError("the B-Rep result is not a valid solid");
                std::vector<double> V;
                std::vector<std::uint32_t> F;
                mesh_shape(s, r.at("chord").get<double>(), V, F);
                payload.resize(V.size() * 8 + F.size() * 4);
                std::memcpy(payload.data(), V.data(), V.size() * 8);
                std::memcpy(payload.data() + V.size() * 8, F.data(), F.size() * 4);
                reply = {{"ok", true}, {"payload", true}, {"nv", V.size() / 3}, {"nt", F.size() / 3}};
            } else if (op == "step_export") {
                TopoDS_Shape s = builder.build(r.at("tree"));
                STEPControl_Writer w;
                Interface_Static::SetCVal("write.step.unit", "MM");
                if (w.Transfer(s, STEPControl_AsIs) != IFSelect_RetDone) throw KernelError("STEP transfer failed");
                std::string path = r.at("path");
                if (w.Write(path.c_str()) != IFSelect_RetDone) throw KernelError("cannot write STEP file '" + path + "'");
                reply = {{"ok", true}};
            } else {
                throw KernelError("unknown request '" + op + "'");
            }
        } catch (const Standard_Failure& e) {
            reply = {{"ok", false}, {"error", std::string("B-Rep kernel: ") + (e.GetMessageString() ? e.GetMessageString() : "failure")}};
            payload.clear();
        } catch (const std::exception& e) {
            reply = {{"ok", false}, {"error", e.what()}};
            payload.clear();
        }
        write_frame(reply.dump());
        if (!payload.empty()) write_frame(payload);
    }
    return 0;
}
