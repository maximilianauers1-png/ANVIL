// glTF 2.0 / OBJ / STL import (see model_io.hpp).
//
// glTF reference: Khronos glTF 2.0 specification, sections 3.5 (nodes and
// hierarchy: world = parent * local, local = T * R * S), 3.6 (binary data
// storage: buffers, buffer views, accessors), 3.7.3 (skins: the skinned vertex
// is sum_i w_i * (jointWorld_i * inverseBind_i) * v; the mesh node's own
// transform is ignored) and 4.4 (GLB container).
#include "anvil/model_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "anvil/stl.hpp"

namespace anvil {

using json = nlohmann::json;

// ------------------------------------------------------------------ Mat4d
Mat4d Mat4d::operator*(const Mat4d& b) const {
    Mat4d r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

Mat4d Mat4d::inverse_affine() const {
    // 3x3 inverse by the adjugate, then t' = -A^-1 t
    const double a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const double det = a * A + b * B + c * C;
    if (std::abs(det) < 1e-300) throw std::runtime_error("singular node matrix");
    const double inv[9] = {A / det, -(b * i - c * h) / det, (b * f - c * e) / det,
                           B / det, (a * i - c * g) / det,  -(a * f - c * d) / det,
                           C / det, -(a * h - b * g) / det, (a * e - b * d) / det};  // row-major
    Mat4d r;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) r.m[col * 4 + row] = inv[row * 3 + col];
    const Vec3 t = translation();
    for (int row = 0; row < 3; ++row)
        r.m[12 + row] = -(inv[row * 3] * t.x + inv[row * 3 + 1] * t.y + inv[row * 3 + 2] * t.z);
    return r;
}

namespace {

constexpr std::uint32_t kGlbMagic = 0x46546C67;  // "glTF"
constexpr std::uint32_t kChunkJson = 0x4E4F534A;  // "JSON"
constexpr std::uint32_t kChunkBin = 0x004E4942;   // "BIN\0"
constexpr double kMetresToMm = 1000.0;

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::uint32_t u32(const std::string& s, std::size_t at) {
    if (at + 4 > s.size()) throw std::runtime_error("GLB: truncated file");
    std::uint32_t v;
    std::memcpy(&v, s.data() + at, 4);
    return v;
}

std::string base64_decode(const std::string& in) {
    static const auto table = [] {
        std::array<int, 256> t{};
        t.fill(-1);
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) t[std::uint8_t(a[i])] = i;
        return t;
    }();
    std::string out;
    out.reserve(in.size() * 3 / 4);
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (table[c] < 0) continue;  // '=', whitespace
        val = (val << 6) | table[c];
        bits += 6;
        if (bits >= 0) {
            out.push_back(char((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

/// The glTF -> ANVIL frame change: (x, y, z) -> (x, -z, y), metres -> mm.
Mat4d frame_change() {
    Mat4d c;
    c.m = {kMetresToMm, 0, 0, 0, 0, 0, kMetresToMm, 0, 0, -kMetresToMm, 0, 0, 0, 0, 0, 1};
    return c;
}

Mat4d trs(const json& node) {
    Mat4d r;
    if (node.contains("matrix")) {
        const auto& a = node["matrix"];
        if (a.size() != 16) throw std::runtime_error("glTF: node matrix must have 16 numbers");
        for (int i = 0; i < 16; ++i) r.m[i] = a[i].get<double>();
        return r;
    }
    Vec3 t{0, 0, 0}, s{1, 1, 1};
    double q[4] = {0, 0, 0, 1};  // x, y, z, w
    if (node.contains("translation")) t = {node["translation"][0], node["translation"][1], node["translation"][2]};
    if (node.contains("scale")) s = {node["scale"][0], node["scale"][1], node["scale"][2]};
    if (node.contains("rotation"))
        for (int i = 0; i < 4; ++i) q[i] = node["rotation"][i].get<double>();
    const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 0)
        for (double& v : q) v /= n;
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    // unit quaternion -> rotation matrix (column-major), columns scaled by s
    const double R[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w),     2 * (x * z - y * w),
                         2 * (x * y - z * w),     1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                         2 * (x * z + y * w),     2 * (y * z - x * w),     1 - 2 * (x * x + y * y)};
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < 3; ++row) r.m[c * 4 + row] = R[c * 3 + row] * s[c];
    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}

/// Removes scale/shear from the 3x3 part (Gram-Schmidt), keeping the translation.
Mat4d rigid(const Mat4d& a) {
    Vec3 x{a.m[0], a.m[1], a.m[2]}, y{a.m[4], a.m[5], a.m[6]};
    x = normalized(x);
    y = normalized(y - x * dot(x, y));
    Vec3 z = cross(x, y);
    Mat4d r;
    r.m = {x.x, x.y, x.z, 0, y.x, y.y, y.z, 0, z.x, z.y, z.z, 0, a.m[12], a.m[13], a.m[14], 1};
    if (norm(x) == 0 || norm(y) == 0) r.m = Mat4d::identity().m, r.m[12] = a.m[12], r.m[13] = a.m[13], r.m[14] = a.m[14];
    return r;
}

struct Gltf {
    json doc;
    std::vector<std::string> buffers;

    /// Reads accessor `index` as `comps` doubles per element (normalized
    /// integers mapped to [0, 1] per spec 3.11).
    std::vector<double> read(int index, int& comps) const {
        const auto& acc = doc.at("accessors").at(index);
        static const std::map<std::string, int> kComps = {{"SCALAR", 1}, {"VEC2", 2}, {"VEC3", 3}, {"VEC4", 4}, {"MAT4", 16}};
        auto it = kComps.find(acc.at("type").get<std::string>());
        if (it == kComps.end()) throw std::runtime_error("glTF: unsupported accessor type " + acc.at("type").get<std::string>());
        comps = it->second;
        const std::size_t count = acc.at("count").get<std::size_t>();
        const int ct = acc.at("componentType").get<int>();
        const bool normalized = acc.value("normalized", false);
        if (acc.contains("sparse")) throw std::runtime_error("glTF: sparse accessors are not supported");
        std::vector<double> out(count * std::size_t(comps), 0.0);
        if (!acc.contains("bufferView")) return out;  // all zeros per spec
        const auto& bv = doc.at("bufferViews").at(acc.at("bufferView").get<int>());
        const std::string& buf = buffers.at(bv.at("buffer").get<std::size_t>());
        const int csize = (ct == 5120 || ct == 5121) ? 1 : (ct == 5122 || ct == 5123) ? 2 : (ct == 5125 || ct == 5126) ? 4 : 0;
        if (csize == 0) throw std::runtime_error("glTF: unsupported component type " + std::to_string(ct));
        const std::size_t stride = bv.value("byteStride", std::size_t(0)) ? bv["byteStride"].get<std::size_t>() : std::size_t(csize * comps);
        const std::size_t base = bv.value("byteOffset", std::size_t(0)) + acc.value("byteOffset", std::size_t(0));
        if (count > 0 && base + (count - 1) * stride + std::size_t(csize * comps) > buf.size())
            throw std::runtime_error("glTF: accessor reads past the end of its buffer");
        const char* p = buf.data() + base;
        for (std::size_t i = 0; i < count; ++i)
            for (int k = 0; k < comps; ++k) {
                const char* q = p + i * stride + std::size_t(k * csize);
                double v = 0;
                switch (ct) {
                    case 5120: { std::int8_t x; std::memcpy(&x, q, 1); v = normalized ? std::max(x / 127.0, -1.0) : x; break; }
                    case 5121: { std::uint8_t x; std::memcpy(&x, q, 1); v = normalized ? x / 255.0 : x; break; }
                    case 5122: { std::int16_t x; std::memcpy(&x, q, 2); v = normalized ? std::max(x / 32767.0, -1.0) : x; break; }
                    case 5123: { std::uint16_t x; std::memcpy(&x, q, 2); v = normalized ? x / 65535.0 : x; break; }
                    case 5125: { std::uint32_t x; std::memcpy(&x, q, 4); v = x; break; }
                    case 5126: { float x; std::memcpy(&x, q, 4); v = x; break; }
                }
                out[i * std::size_t(comps) + std::size_t(k)] = v;
            }
        return out;
    }
};

}  // namespace

ModelData decode_gltf_json(const std::string& json_text, const std::string& bin_chunk, const std::filesystem::path& base_dir) {
    Gltf g;
    try {
        g.doc = json::parse(json_text);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("glTF: invalid JSON: ") + e.what());
    }
    const json& d = g.doc;
    for (const char* key : {"extensionsRequired"})
        if (d.contains(key))
            for (const auto& ext : d[key]) {
                const std::string name = ext.get<std::string>();
                if (name == "KHR_draco_mesh_compression" || name == "EXT_meshopt_compression" || name == "KHR_mesh_quantization")
                    throw std::runtime_error("glTF: the file uses " + name +
                                             ", which ANVIL cannot decode; export the model without mesh compression");
            }
    // buffers
    if (d.contains("buffers"))
        for (std::size_t i = 0; i < d["buffers"].size(); ++i) {
            const auto& b = d["buffers"][i];
            if (!b.contains("uri")) {
                if (i != 0) throw std::runtime_error("glTF: only buffer 0 may omit its uri (GLB)");
                g.buffers.push_back(bin_chunk);
                continue;
            }
            const std::string uri = b["uri"].get<std::string>();
            if (uri.rfind("data:", 0) == 0) {
                const auto comma = uri.find(',');
                if (comma == std::string::npos || uri.substr(0, comma).find(";base64") == std::string::npos)
                    throw std::runtime_error("glTF: unsupported data URI");
                g.buffers.push_back(base64_decode(uri.substr(comma + 1)));
            } else {
                if (uri.find("://") != std::string::npos) throw std::runtime_error("glTF: remote buffer URIs are not allowed (offline)");
                g.buffers.push_back(read_file(base_dir / std::filesystem::path(std::u8string(uri.begin(), uri.end()))));
            }
        }
    const json empty = json::array();
    const json& nodes = d.contains("nodes") ? d["nodes"] : empty;
    const std::size_t nn = nodes.size();
    std::vector<int> parent(nn, -1);
    for (std::size_t i = 0; i < nn; ++i)
        if (nodes[i].contains("children"))
            for (const auto& c : nodes[i]["children"]) parent.at(c.get<std::size_t>()) = int(i);
    // world transforms (glTF frame), computed over the hierarchy
    std::vector<Mat4d> world(nn);
    std::vector<char> done(nn, 0);
    std::function<const Mat4d&(std::size_t)> world_of = [&](std::size_t i) -> const Mat4d& {
        if (!done[i]) {
            world[i] = parent[i] < 0 ? trs(nodes[i]) : world_of(std::size_t(parent[i])) * trs(nodes[i]);
            done[i] = 1;
        }
        return world[i];
    };
    // nodes of the active scene (all roots when no scene is given)
    std::vector<std::size_t> active;
    {
        std::vector<std::size_t> stack;
        if (d.contains("scenes") && !d["scenes"].empty()) {
            const std::size_t s = d.value("scene", std::size_t(0));
            for (const auto& r : d["scenes"].at(s).value("nodes", json::array())) stack.push_back(r.get<std::size_t>());
        } else {
            for (std::size_t i = 0; i < nn; ++i)
                if (parent[i] < 0) stack.push_back(i);
        }
        while (!stack.empty()) {
            std::size_t i = stack.back();
            stack.pop_back();
            active.push_back(i);
            if (nodes[i].contains("children"))
                for (const auto& c : nodes[i]["children"]) stack.push_back(c.get<std::size_t>());
        }
        std::sort(active.begin(), active.end());
    }
    // joints: every node used by a skin of an active node, in node order
    std::vector<int> joint_of(nn, -1);
    std::vector<std::size_t> joint_nodes;
    for (std::size_t i : active)
        if (nodes[i].contains("skin") && nodes[i].contains("mesh"))
            for (const auto& j : d.at("skins").at(nodes[i]["skin"].get<std::size_t>()).at("joints")) {
                const auto jn = j.get<std::size_t>();
                if (joint_of.at(jn) < 0) {
                    joint_of[jn] = 0;
                    joint_nodes.push_back(jn);
                }
            }
    std::sort(joint_nodes.begin(), joint_nodes.end());
    for (std::size_t k = 0; k < joint_nodes.size(); ++k) joint_of[joint_nodes[k]] = int(k);
    auto nearest_joint = [&](std::size_t n) {
        for (int p = int(n); p >= 0; p = parent[std::size_t(p)])
            if (joint_of[std::size_t(p)] >= 0) return joint_of[std::size_t(p)];
        return -1;
    };

    const Mat4d C = frame_change(), Ci = C.inverse_affine();
    ModelData md;
    md.format = bin_chunk.empty() ? "gltf" : "glb";
    auto skin = std::make_shared<Skin>();
    const bool skinned = !joint_nodes.empty();
    for (std::size_t i : active) {
        const auto& node = nodes[i];
        if (!node.contains("mesh")) continue;
        const auto& mesh = d.at("meshes").at(node["mesh"].get<std::size_t>());
        // skin matrices of this node (glTF frame), or its rigid world transform
        std::vector<Mat4d> skin_mats;
        std::vector<int> local_to_global;
        if (node.contains("skin")) {
            const auto& sk = d["skins"].at(node["skin"].get<std::size_t>());
            const auto& js = sk.at("joints");
            std::vector<double> ibm;
            int comps = 16;
            if (sk.contains("inverseBindMatrices")) ibm = g.read(sk["inverseBindMatrices"].get<int>(), comps);
            for (std::size_t k = 0; k < js.size(); ++k) {
                Mat4d ib;
                if (!ibm.empty())
                    for (int e = 0; e < 16; ++e) ib.m[e] = ibm.at(k * 16 + std::size_t(e));
                skin_mats.push_back(world_of(js[k].get<std::size_t>()) * ib);
                local_to_global.push_back(joint_of[js[k].get<std::size_t>()]);
            }
        }
        const Mat4d W = world_of(i);
        const int rigid_joint = nearest_joint(i);
        for (const auto& prim : mesh.at("primitives")) {
            const int mode = prim.value("mode", 4);
            if (mode != 4 && mode != 5 && mode != 6) continue;  // points / lines carry no surface
            const auto& attr = prim.at("attributes");
            if (!attr.contains("POSITION")) continue;
            int pc = 0;
            const auto pos = g.read(attr["POSITION"].get<int>(), pc);
            if (pc != 3) throw std::runtime_error("glTF: POSITION must be VEC3");
            const std::size_t nv = pos.size() / 3;
            std::vector<double> jv, wv;
            int jc = 0, wc = 0;
            const bool has_skin = !skin_mats.empty() && attr.contains("JOINTS_0") && attr.contains("WEIGHTS_0");
            if (has_skin) {
                jv = g.read(attr["JOINTS_0"].get<int>(), jc);
                wv = g.read(attr["WEIGHTS_0"].get<int>(), wc);
                if (jc != 4 || wc != 4 || jv.size() != nv * 4 || wv.size() != nv * 4)
                    throw std::runtime_error("glTF: JOINTS_0/WEIGHTS_0 must be VEC4 per vertex");
            }
            const auto base = std::uint32_t(md.mesh.V.size());
            for (std::size_t v = 0; v < nv; ++v) {
                const Vec3 p{pos[v * 3], pos[v * 3 + 1], pos[v * 3 + 2]};
                std::array<std::uint16_t, 4> jj{0, 0, 0, 0};
                std::array<float, 4> ww{0, 0, 0, 0};
                Vec3 q;
                if (has_skin) {
                    double wsum = 0;
                    for (int k = 0; k < 4; ++k) wsum += wv[v * 4 + std::size_t(k)];
                    Vec3 acc{0, 0, 0};
                    for (int k = 0; k < 4; ++k) {
                        const double w = wsum > 0 ? wv[v * 4 + std::size_t(k)] / wsum : 0.0;
                        const auto lj = std::size_t(jv[v * 4 + std::size_t(k)]);
                        if (w <= 0 || lj >= skin_mats.size()) continue;
                        acc += skin_mats[lj].point(p) * w;
                        jj[std::size_t(k)] = std::uint16_t(std::max(local_to_global[lj], 0));
                        ww[std::size_t(k)] = float(w);
                    }
                    q = wsum > 0 ? acc : W.point(p);
                } else {
                    q = W.point(p);
                    if (rigid_joint >= 0) jj[0] = std::uint16_t(rigid_joint), ww[0] = 1.0f;
                }
                md.mesh.V.push_back(C.point(q));
                if (skinned) {
                    skin->joints.push_back(jj);
                    skin->weights.push_back(ww);
                }
            }
            std::vector<std::uint32_t> idx;
            if (prim.contains("indices")) {
                int ic = 0;
                for (double x : g.read(prim["indices"].get<int>(), ic)) idx.push_back(std::uint32_t(x));
            } else {
                for (std::size_t v = 0; v < nv; ++v) idx.push_back(std::uint32_t(v));
            }
            // a negative determinant mirrors the node: flip the winding (spec 3.7.4)
            const double det = W.m[0] * (W.m[5] * W.m[10] - W.m[9] * W.m[6]) - W.m[4] * (W.m[1] * W.m[10] - W.m[9] * W.m[2]) +
                               W.m[8] * (W.m[1] * W.m[6] - W.m[5] * W.m[2]);
            const bool flip = !has_skin && det < 0;
            auto add = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
                if (a >= nv || b >= nv || c >= nv) throw std::runtime_error("glTF: index out of range");
                if (a == b || b == c || a == c) return;
                if (flip) std::swap(b, c);
                md.mesh.F.push_back({base + a, base + b, base + c});
            };
            if (mode == 4) {
                for (std::size_t t = 0; t + 2 < idx.size(); t += 3) add(idx[t], idx[t + 1], idx[t + 2]);
            } else if (mode == 5) {
                for (std::size_t t = 0; t + 2 < idx.size(); ++t)
                    (t % 2 == 0) ? add(idx[t], idx[t + 1], idx[t + 2]) : add(idx[t + 1], idx[t], idx[t + 2]);
            } else {
                for (std::size_t t = 1; t + 1 < idx.size(); ++t) add(idx[0], idx[t], idx[t + 1]);
            }
            ++md.parts;
        }
    }
    if (md.mesh.F.empty()) throw std::runtime_error("glTF: the file contains no triangles");
    if (skinned) {
        for (std::size_t k = 0; k < joint_nodes.size(); ++k) {
            const std::size_t n = joint_nodes[k];
            skin->joint_names.push_back(nodes[n].value("name", "joint" + std::to_string(k)));
            int p = parent[n];
            while (p >= 0 && joint_of[std::size_t(p)] < 0) p = parent[std::size_t(p)];
            skin->parent.push_back(p < 0 ? -1 : joint_of[std::size_t(p)]);
            skin->rest_world.push_back(rigid(C * world_of(n) * Ci));
        }
        md.skin = skin;
    }
    return md;
}

ModelData decode_glb(const std::string& bytes, const std::filesystem::path& base_dir) {
    if (bytes.size() < 20 || u32(bytes, 0) != kGlbMagic) throw std::runtime_error("GLB: not a binary glTF file");
    if (u32(bytes, 4) != 2) throw std::runtime_error("GLB: only glTF 2.0 is supported");
    const std::size_t total = std::min<std::size_t>(u32(bytes, 8), bytes.size());
    std::string json_text, bin;
    for (std::size_t at = 12; at + 8 <= total;) {
        const std::size_t len = u32(bytes, at);
        const std::uint32_t type = u32(bytes, at + 4);
        if (at + 8 + len > total) throw std::runtime_error("GLB: truncated chunk");
        if (type == kChunkJson && json_text.empty()) json_text = bytes.substr(at + 8, len);
        else if (type == kChunkBin && bin.empty()) bin = bytes.substr(at + 8, len);
        at += 8 + ((len + 3) & ~std::size_t(3));
    }
    if (json_text.empty()) throw std::runtime_error("GLB: missing JSON chunk");
    ModelData md = decode_gltf_json(json_text, bin, base_dir);
    md.format = "glb";
    return md;
}

ModelData decode_obj(const std::string& text) {
    // Wavefront OBJ: "v x y z", "f a[/t][/n] b c ..." (1-based, negative =
    // relative). Polygons are fan-triangulated. Y up as written by Tripo,
    // Meshy and Blender's default export -> (x, -z, y); units kept.
    ModelData md;
    md.format = "obj";
    std::istringstream in(text);
    std::string line;
    std::vector<std::uint32_t> poly;
    while (std::getline(in, line)) {
        if (line.size() < 2) continue;
        if (line[0] == 'v' && (line[1] == ' ' || line[1] == '\t')) {
            std::istringstream ls(line.substr(2));
            double x = 0, y = 0, z = 0;
            ls >> x >> y >> z;
            md.mesh.V.push_back({x, -z, y});
        } else if (line[0] == 'f' && (line[1] == ' ' || line[1] == '\t')) {
            std::istringstream ls(line.substr(2));
            std::string tok;
            poly.clear();
            while (ls >> tok) {
                long i = std::stol(tok.substr(0, tok.find('/')));
                const long n = long(md.mesh.V.size());
                if (i < 0) i += n + 1;
                if (i < 1 || i > n) throw std::runtime_error("OBJ: face index out of range: " + line);
                poly.push_back(std::uint32_t(i - 1));
            }
            for (std::size_t k = 1; k + 1 < poly.size(); ++k)
                if (poly[0] != poly[k] && poly[k] != poly[k + 1] && poly[0] != poly[k + 1])
                    md.mesh.F.push_back({poly[0], poly[k], poly[k + 1]});
        } else if (line[0] == 'o' || line[0] == 'g') {
            ++md.parts;
        }
    }
    if (md.mesh.F.empty()) throw std::runtime_error("OBJ: the file contains no faces");
    md.parts = std::max<std::size_t>(md.parts, 1);
    return md;
}

namespace {
/// Merges vertices with identical positions (seams split for UVs/normals);
/// skin data of the first copy is kept.
void weld_positions(ModelData& md) {
    struct Key {
        double x, y, z;
        bool operator==(const Key&) const = default;
    };
    struct H {
        std::size_t operator()(const Key& k) const {
            auto h = [](double v) { return std::hash<double>()(v); };
            return h(k.x) ^ (h(k.y) * 0x9E3779B97F4A7C15ull) ^ (h(k.z) * 0xC2B2AE3D27D4EB4Full);
        }
    };
    std::unordered_map<Key, std::uint32_t, H> seen;
    std::vector<std::uint32_t> remap(md.mesh.V.size());
    Mesh out;
    auto skin = md.skin ? std::make_shared<Skin>(*md.skin) : nullptr;
    if (skin) skin->joints.clear(), skin->weights.clear();
    for (std::size_t i = 0; i < md.mesh.V.size(); ++i) {
        const Vec3& p = md.mesh.V[i];
        auto [it, inserted] = seen.emplace(Key{p.x, p.y, p.z}, std::uint32_t(out.V.size()));
        if (inserted) {
            out.V.push_back(p);
            if (skin) skin->joints.push_back(md.skin->joints[i]), skin->weights.push_back(md.skin->weights[i]);
        }
        remap[i] = it->second;
    }
    for (const auto& f : md.mesh.F) {
        Tri t{remap[f[0]], remap[f[1]], remap[f[2]]};
        if (t[0] != t[1] && t[1] != t[2] && t[0] != t[2]) out.F.push_back(t);
    }
    md.mesh = std::move(out);
    if (skin) md.skin = skin;
}
}  // namespace

ModelData read_model(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const std::string bytes = read_file(path);
    ModelData md;
    if (ext == ".glb") {
        md = decode_glb(bytes, path.parent_path());
    } else if (ext == ".gltf") {
        md = decode_gltf_json(bytes, {}, path.parent_path());
    } else if (ext == ".obj") {
        md = decode_obj(bytes);
    } else if (ext == ".stl") {
        md.mesh = decode_stl(bytes).mesh;
        md.format = "stl";
        md.parts = 1;
    } else {
        throw std::runtime_error("unsupported model format '" + ext + "' (use .glb, .gltf, .obj or .stl)");
    }
    weld_positions(md);
    return md;
}

double fit_to_height(ModelData& md, double height_mm) {
    const Box3 b = md.mesh.bounds();
    const double hz = b.hi.z - b.lo.z;
    if (!(hz > 0) || !(height_mm > 0)) throw std::runtime_error("fit_to_height: model has no height");
    const double s = height_mm / hz;
    const Vec3 shift{-(b.lo.x + b.hi.x) * 0.5 * s, -(b.lo.y + b.hi.y) * 0.5 * s, -b.lo.z * s};
    for (auto& v : md.mesh.V) v = v * s + shift;
    if (md.skin) {
        auto sk = std::make_shared<Skin>(*md.skin);
        for (auto& w : sk->rest_world) {
            w.m[12] = w.m[12] * s + shift.x;
            w.m[13] = w.m[13] * s + shift.y;
            w.m[14] = w.m[14] * s + shift.z;
        }
        md.skin = sk;
    }
    return s;
}

}  // namespace anvil
