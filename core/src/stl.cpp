#include "anvil/stl.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "anvil/fsutil.hpp"

namespace anvil {

// ---------------------------------------------------------------- SHA-256
namespace {
struct Sha256 {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::uint64_t len = 0;
    unsigned char buf[64];
    std::size_t n = 0;
    static std::uint32_t rotr(std::uint32_t x, int k) { return (x >> k) | (x << (32 - k)); }
    void block(const unsigned char* p) {
        static const std::uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = std::uint32_t(p[4 * i]) << 24 | std::uint32_t(p[4 * i + 1]) << 16 |
                   std::uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t t2 = S0 + mj;
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }
    void update(const unsigned char* p, std::size_t k) {
        len += k;
        while (k) {
            std::size_t t = std::min(k, 64 - n);
            std::memcpy(buf + n, p, t);
            n += t, p += t, k -= t;
            if (n == 64) block(buf), n = 0;
        }
    }
    std::string hex() {
        std::uint64_t bits = len * 8;
        unsigned char pad = 0x80, zero = 0;
        update(&pad, 1);
        while (n != 56) update(&zero, 1);
        unsigned char lb[8];
        for (int i = 0; i < 8; ++i) lb[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
        update(lb, 8);
        static const char* hx = "0123456789abcdef";
        std::string s;
        for (auto v : h)
            for (int i = 3; i >= 0; --i) {
                unsigned b = (v >> (8 * i)) & 0xff;
                s += hx[b >> 4], s += hx[b & 15];
            }
        return s;
    }
};

void put_f32(std::string& s, float f) {
    static_assert(sizeof(float) == 4);
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    for (int i = 0; i < 4; ++i) s += static_cast<char>((u >> (8 * i)) & 0xff);  // little endian
}
float get_f32(const unsigned char* p) {
    std::uint32_t u = std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 |
                      std::uint32_t(p[3]) << 24;
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
}  // namespace

std::string sha256_hex(const std::string& bytes) {
    Sha256 s;
    s.update(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    return s.hex();
}

// ---------------------------------------------------------------- write
std::string encode_stl(const Mesh& m, const StlWriteOptions& opt) {
    if (m.F.empty()) throw std::runtime_error("cannot write an empty mesh");
    const double k = 1.0 / unit_info(opt.unit).mm_per_unit;
    std::string hdr = "ANVIL binary STL unit=" + std::string(unit_info(opt.unit).symbol);
    if (!opt.note.empty()) hdr += " " + opt.note;
    if (opt.ascii) {
        std::ostringstream o;
        o.imbue(std::locale::classic());
        o << "solid anvil_unit_" << unit_info(opt.unit).symbol << "\n";
        char buf[256];
        for (const auto& t : m.F) {
            Vec3 a = m.V[t[0]] * k, b = m.V[t[1]] * k, c = m.V[t[2]] * k;
            Vec3 n = normalized(cross(b - a, c - a));
            std::snprintf(buf, sizeof buf, "  facet normal %.9g %.9g %.9g\n    outer loop\n", n.x, n.y, n.z);
            o << buf;
            for (const Vec3* p : {&a, &b, &c}) {
                std::snprintf(buf, sizeof buf, "      vertex %.9g %.9g %.9g\n", float(p->x), float(p->y),
                              float(p->z));
                o << buf;
            }
            o << "    endloop\n  endfacet\n";
        }
        o << "endsolid anvil_unit_" << unit_info(opt.unit).symbol << "\n";
        return o.str();
    }
    if (hdr.size() > 80) hdr.resize(80);
    std::string s = hdr;
    s.resize(80, ' ');
    const auto n = static_cast<std::uint32_t>(m.F.size());
    s.reserve(84 + 50ull * n);
    for (int i = 0; i < 4; ++i) s += static_cast<char>((n >> (8 * i)) & 0xff);
    for (const auto& t : m.F) {
        Vec3 a = m.V[t[0]] * k, b = m.V[t[1]] * k, c = m.V[t[2]] * k;
        Vec3 nn = normalized(cross(b - a, c - a));
        put_f32(s, float(nn.x)), put_f32(s, float(nn.y)), put_f32(s, float(nn.z));
        for (const Vec3* p : {&a, &b, &c}) put_f32(s, float(p->x)), put_f32(s, float(p->y)), put_f32(s, float(p->z));
        s += '\0', s += '\0';
    }
    return s;
}

std::string write_stl(const std::filesystem::path& path, const Mesh& m, const StlWriteOptions& opt) {
    std::string bytes = encode_stl(m, opt);
    write_file_atomic(path, bytes);
    return sha256_hex(bytes);
}

// ---------------------------------------------------------------- read
namespace {
struct Welder {
    struct Key {
        float x, y, z;
        bool operator==(const Key& o) const { return std::memcmp(this, &o, sizeof(Key)) == 0; }
    };
    struct Hash {
        std::size_t operator()(const Key& k) const {
            std::uint32_t u[3];
            std::memcpy(u, &k, 12);
            return (std::size_t(u[0]) * 73856093u) ^ (std::size_t(u[1]) * 19349663u) ^ (std::size_t(u[2]) * 83492791u);
        }
    };
    std::unordered_map<Key, std::uint32_t, Hash> map;
    Mesh mesh;
    double scale = 1.0;
    std::size_t dropped = 0;
    std::uint32_t vert(float x, float y, float z) {
        if (x == 0.0f) x = 0.0f;  // fold -0
        if (y == 0.0f) y = 0.0f;
        if (z == 0.0f) z = 0.0f;
        auto [it, ins] = map.try_emplace(Key{x, y, z}, std::uint32_t(mesh.V.size()));
        if (ins) mesh.V.push_back({double(x) * scale, double(y) * scale, double(z) * scale});
        return it->second;
    }
    void tri(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        if (a == b || b == c || a == c) {
            ++dropped;
            return;
        }
        mesh.F.push_back({a, b, c});
    }
};
}  // namespace

StlReadResult decode_stl(const std::string& bytes, LengthUnit unit) {
    StlReadResult r;
    Welder w;
    w.scale = unit_info(unit).mm_per_unit;
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
    bool binary = false;
    if (bytes.size() >= 84) {
        std::uint32_t n = std::uint32_t(p[80]) | std::uint32_t(p[81]) << 8 | std::uint32_t(p[82]) << 16 |
                          std::uint32_t(p[83]) << 24;
        binary = bytes.size() == 84 + 50ull * n;  // decisive: many binary files start with "solid"
        if (binary) {
            r.header.assign(bytes.data(), 80);
            r.raw_triangles = n;
            w.mesh.F.reserve(n);
            w.mesh.V.reserve(n / 2 + 16);   // closed meshes: V ~ F/2 (Euler)
            w.map.reserve(n / 2 + 16);
            for (std::uint32_t i = 0; i < n; ++i) {
                const unsigned char* q = p + 84 + 50ull * i + 12;
                std::uint32_t id[3];
                for (int k = 0; k < 3; ++k)
                    id[k] = w.vert(get_f32(q + 12 * k), get_f32(q + 12 * k + 4), get_f32(q + 12 * k + 8));
                w.tri(id[0], id[1], id[2]);
            }
        }
    }
    if (!binary) {
        if (bytes.compare(0, 5, "solid") != 0)
            throw std::runtime_error("not an STL file (size does not match 84 + 50*N and no 'solid' keyword)");
        r.ascii = true;
        std::istringstream in(bytes);
        in.imbue(std::locale::classic());
        std::getline(in, r.header);
        std::string tok;
        std::uint32_t id[3];
        int k = 0;
        while (in >> tok) {
            if (tok == "vertex") {
                float x, y, z;
                if (!(in >> x >> y >> z)) throw std::runtime_error("malformed ASCII STL vertex");
                if (k < 3) id[k] = w.vert(x, y, z);
                ++k;
            } else if (tok == "endloop") {
                if (k != 3) throw std::runtime_error("ASCII STL facet without exactly 3 vertices");
                w.tri(id[0], id[1], id[2]);
                ++r.raw_triangles;
                k = 0;
            }
        }
    }
    r.mesh = std::move(w.mesh);
    r.dropped_degenerate = w.dropped;
    return r;
}

StlReadResult read_stl(const std::filesystem::path& path, LengthUnit unit) {
    return decode_stl(read_file(path), unit);
}

}  // namespace anvil
