#include "anvil/geom_client.hpp"

#include <cstdlib>
#include <cstring>

#include <nlohmann/json.hpp>

#include "anvil/geometry.hpp"
#include "subprocess.hpp"

namespace anvil {

namespace {
std::string frame(const std::string& payload) {
    std::uint32_t n = std::uint32_t(payload.size());
    std::string out(4, '\0');
    for (int i = 0; i < 4; ++i) out[i] = char((n >> (8 * i)) & 0xff);
    return out + payload;
}
}  // namespace

std::filesystem::path default_geom_worker(const std::filesystem::path& exe_dir) {
    if (const char* env = std::getenv("ANVIL_GEOM")) return env;
#ifdef _WIN32
    return exe_dir / "anvil-geom.exe";
#else
    return exe_dir / "anvil-geom";
#endif
}

GeomClient::GeomClient(std::filesystem::path exe, std::chrono::milliseconds timeout)
    : exe_(std::move(exe)), timeout_(timeout) {}

GeomClient::~GeomClient() = default;

void GeomClient::ensure_running() {
    if (proc_ && proc_->alive()) return;
    if (!std::filesystem::exists(exe_))
        throw GeometryError("B-Rep kernel worker not found: " + exe_.string());
    proc_.reset();
    proc_ = std::make_unique<Subprocess>(exe_);
    ++starts_;
}

std::string GeomClient::call(const std::string& request, std::string* payload) {
    ensure_running();
    auto fail = [&](const std::string& why) -> std::string {
        proc_->kill();
        proc_.reset();  // restarted on the next call
        throw GeometryError(why);
    };
    try {
        proc_->write(frame(request));
    } catch (const std::exception&) {
        fail("the B-Rep kernel worker crashed (pipe closed); it will be restarted");
    }
    auto read_frame = [&](std::string& out) {
        std::string len;
        bool timed_out = false;
        if (!proc_->read(4, len, timeout_, &timed_out))
            fail(timed_out ? "the B-Rep kernel did not answer within " + std::to_string(timeout_.count() / 1000) +
                                 " s and was stopped (operation too complex?)"
                           : "the B-Rep kernel worker crashed while computing this feature; it will be restarted");
        std::uint32_t n = 0;
        for (int i = 0; i < 4; ++i) n |= std::uint32_t(std::uint8_t(len[i])) << (8 * i);
        if (!proc_->read(n, out, timeout_, &timed_out)) fail("the B-Rep kernel worker stopped while sending a result");
    };
    std::string header;
    read_frame(header);
    auto h = nlohmann::json::parse(header);
    if (!h.value("ok", false)) throw GeometryError(h.value("error", std::string("B-Rep kernel error")));
    if (payload && h.value("payload", false)) read_frame(*payload);
    return header;
}

std::shared_ptr<const Mesh> GeomClient::eval(const std::string& tree, double chord) {
    std::lock_guard lk(mu_);
    std::string req = R"({"op":"eval","chord":)" + std::to_string(chord) + R"(,"tree":)" + tree + "}";
    std::string payload;
    auto h = nlohmann::json::parse(call(req, &payload));
    const std::size_t nv = h.at("nv"), nt = h.at("nt");
    if (payload.size() != nv * 24 + nt * 12) throw GeometryError("B-Rep kernel sent a malformed mesh");
    auto m = std::make_shared<Mesh>();
    m->V.resize(nv);
    m->F.resize(nt);
    std::memcpy(m->V.data(), payload.data(), nv * 24);  // Vec3 is 3 doubles
    std::memcpy(m->F.data(), payload.data() + nv * 24, nt * 12);
    return m;
}

void GeomClient::export_step(const std::string& tree, const std::filesystem::path& path) {
    std::lock_guard lk(mu_);
    auto u8 = path.u8string();
    nlohmann::json req{{"op", "step_export"}, {"path", std::string(u8.begin(), u8.end())}};
    std::string r = req.dump();
    r.insert(r.size() - 1, R"(,"tree":)" + tree);
    call(r, nullptr);
}

void GeomClient::debug_crash() {
    std::lock_guard lk(mu_);
    try {
        call(R"({"op":"crash"})", nullptr);
    } catch (const GeometryError&) {
    }
}

}  // namespace anvil
