// anvil-ai: the only ANVIL process that opens a socket (ADR 0001, 0010). It
// talks to a local Ollama server over HTTP on the loopback interface and to
// the app over JSON lines on stdin/stdout:
//
//   {"id": 1, "op": "status"}                      -> {"id": 1, "event": "result", "running": true, "version": "...", "models": [...]}
//   {"id": 2, "op": "chat", "request": {...}}      -> {"id": 2, "event": "result", "response": {...}}   (Ollama /api/chat body)
//   {"id": 3, "op": "pull", "model": "qwen2.5:7b"} -> {"id": 3, "event": "progress", "status": "...", "completed": n, "total": n} ...
//                                                     {"id": 3, "event": "result", "ok": true}
//   {"id": 4, "op": "delete", "model": "..."}      -> {"id": 4, "event": "result", "ok": true}
//   errors: {"id": n, "event": "error", "message": "..."}
//
// The server address comes from ANVIL_OLLAMA_URL (default http://127.0.0.1:11434;
// OLLAMA_HOST is honoured too). Anything that is not a loopback address is
// refused: ANVIL never sends data off the machine. The model downloads
// (pull) are performed by Ollama itself, after the user started them in
// ANVIL's installer dialog (ADR 0010 4.).
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
static void close_sock(sock_t s) { closesocket(s); }
static constexpr sock_t kBad = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using sock_t = int;
static void close_sock(sock_t s) { close(s); }
static constexpr sock_t kBad = -1;
#endif

using json = nlohmann::json;

namespace {

constexpr int kTimeoutSeconds = 900;  // a first chat loads the model into VRAM (can take minutes)

struct Endpoint {
    bool v6 = false;
    std::string host;  // numeric
    int port = 11434;
};

// "http://127.0.0.1:11434", "127.0.0.1:11434", "localhost", "[::1]:11434" -> loopback endpoint, else error
Endpoint parse_endpoint(std::string s) {
    if (s.rfind("http://", 0) == 0) s = s.substr(7);
    if (s.rfind("https://", 0) == 0) throw std::runtime_error("https is not used for the local server: " + s);
    if (auto slash = s.find('/'); slash != std::string::npos) s = s.substr(0, slash);
    Endpoint e;
    std::string host = s;
    if (!s.empty() && s[0] == '[') {
        const auto close = s.find(']');
        host = s.substr(1, close - 1);
        if (close + 1 < s.size() && s[close + 1] == ':') e.port = std::stoi(s.substr(close + 2));
        e.v6 = true;
    } else if (auto colon = s.rfind(':'); colon != std::string::npos) {
        host = s.substr(0, colon);
        e.port = std::stoi(s.substr(colon + 1));
    }
    if (host.empty() || host == "localhost" || host == "127.0.0.1") e.host = "127.0.0.1", e.v6 = false;
    else if (host == "::1") e.host = "::1", e.v6 = true;
    else if (host.rfind("127.", 0) == 0) e.host = host;  // 127.0.0.0/8 is loopback
    else
        throw std::runtime_error("refusing non-loopback AI server '" + host +
                                 "': ANVIL only talks to a local server (127.0.0.1); check OLLAMA_HOST / ANVIL_OLLAMA_URL");
    if (e.port <= 0 || e.port > 65535) throw std::runtime_error("bad port in '" + s + "'");
    return e;
}

Endpoint endpoint() {
    if (const char* u = std::getenv("ANVIL_OLLAMA_URL"); u && *u) return parse_endpoint(u);
    if (const char* h = std::getenv("OLLAMA_HOST"); h && *h) {
        std::string s = h;
        if (s.rfind("0.0.0.0", 0) == 0) s = "127.0.0.1" + s.substr(7);  // "listen on all": we connect locally
        return parse_endpoint(s);
    }
    return parse_endpoint("127.0.0.1:11434");
}

struct HttpResult {
    int status = 0;
    std::string body;  // when not streamed
};

// Minimal HTTP/1.1 client (Connection: close; chunked transfer decoded).
// on_line: called per NDJSON line of a streamed body (Ollama streams).
HttpResult http(const std::string& method, const std::string& path, const std::string& body,
                const std::function<void(const std::string&)>& on_line = {}) {
    const Endpoint ep = endpoint();
    sock_t s = socket(ep.v6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kBad) throw std::runtime_error("socket() failed");
#ifdef _WIN32
    DWORD tv = kTimeoutSeconds * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof tv);
#else
    timeval tv{kTimeoutSeconds, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
    int rc;
    if (ep.v6) {
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_port = htons(std::uint16_t(ep.port));
        inet_pton(AF_INET6, ep.host.c_str(), &a.sin6_addr);
        rc = connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    } else {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(std::uint16_t(ep.port));
        inet_pton(AF_INET, ep.host.c_str(), &a.sin_addr);
        rc = connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    }
    if (rc != 0) {
        close_sock(s);
        throw std::runtime_error("no local AI server at " + ep.host + ":" + std::to_string(ep.port) +
                                 " (start Ollama; see the AI panel)");
    }
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + ep.host + ":" + std::to_string(ep.port) +
                      "\r\nUser-Agent: anvil-ai\r\nConnection: close\r\nAccept: application/json\r\n";
    if (!body.empty() || method == "POST" || method == "DELETE")
        req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    for (std::size_t off = 0; off < req.size();) {
        const int n = send(s, req.data() + off, int(req.size() - off), 0);
        if (n <= 0) {
            close_sock(s);
            throw std::runtime_error("send() failed");
        }
        off += std::size_t(n);
    }
    std::string raw, header;
    bool have_header = false, chunked = false;
    HttpResult res;
    std::string decoded, line_buf;
    std::size_t pos = 0;  // parse position in raw (body part)
    auto feed = [&](const std::string& data) {
        if (!on_line) {
            decoded += data;
            return;
        }
        line_buf += data;
        for (std::size_t nl; (nl = line_buf.find('\n')) != std::string::npos;) {
            std::string l = line_buf.substr(0, nl);
            line_buf.erase(0, nl + 1);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (!l.empty()) on_line(l);
        }
    };
    auto drain = [&]() {  // consume complete pieces of raw[pos..]
        if (!chunked) {
            feed(raw.substr(pos));
            pos = raw.size();
            return;
        }
        for (;;) {
            const auto crlf = raw.find("\r\n", pos);
            if (crlf == std::string::npos) return;
            const std::size_t len = std::stoul(raw.substr(pos, crlf - pos), nullptr, 16);
            if (len == 0) {
                pos = raw.size();
                return;
            }
            if (raw.size() < crlf + 2 + len + 2) return;
            feed(raw.substr(crlf + 2, len));
            pos = crlf + 2 + len + 2;
        }
    };
    char buf[16384];
    for (;;) {
        const int n = recv(s, buf, sizeof buf, 0);
        if (n < 0) {
            close_sock(s);
            throw std::runtime_error("the local AI server did not answer in time");
        }
        if (n == 0) break;
        raw.append(buf, std::size_t(n));
        if (!have_header) {
            const auto end = raw.find("\r\n\r\n");
            if (end == std::string::npos) continue;
            header = raw.substr(0, end);
            have_header = true;
            pos = end + 4;
            res.status = std::stoi(header.substr(header.find(' ') + 1, 3));
            std::string lower = header;
            for (auto& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
            chunked = lower.find("transfer-encoding: chunked") != std::string::npos;
        }
        drain();
    }
    close_sock(s);
    if (!have_header) throw std::runtime_error("the local AI server closed the connection without an answer");
    drain();
    if (on_line && !line_buf.empty()) on_line(line_buf);
    res.body = decoded;
    return res;
}

void emit(const json& j) {
    std::cout << j.dump() << "\n";
    std::cout.flush();
}

void handle(const json& req) {
    const auto id = req.value("id", json());
    const std::string op = req.value("op", "");
    try {
        if (op == "status") {
            json out{{"id", id}, {"event", "result"}};
            try {
                const auto v = http("GET", "/api/version", "");
                out["running"] = v.status == 200;
                out["version"] = json::parse(v.body).value("version", "");
                const auto t = http("GET", "/api/tags", "");
                out["models"] = json::parse(t.body).value("models", json::array());
            } catch (const std::exception& e) {
                out["running"] = false;
                out["message"] = e.what();
            }
            emit(out);
        } else if (op == "chat") {
            json body = req.at("request");
            body["stream"] = false;
            const auto r = http("POST", "/api/chat", body.dump());
            if (r.status != 200) {
                std::string msg = r.body;
                try { msg = json::parse(r.body).value("error", r.body); } catch (...) {}
                throw std::runtime_error("AI server: " + msg);
            }
            emit({{"id", id}, {"event", "result"}, {"response", json::parse(r.body)}});
        } else if (op == "pull") {
            const std::string model = req.at("model").get<std::string>();
            std::string err;
            const auto r = http("POST", "/api/pull", json{{"model", model}, {"stream", true}}.dump(), [&](const std::string& l) {
                json p;
                try { p = json::parse(l); } catch (...) { return; }
                if (p.contains("error")) err = p["error"].get<std::string>();
                json ev{{"id", id}, {"event", "progress"}, {"status", p.value("status", "")}};
                if (p.contains("completed")) ev["completed"] = p["completed"];
                if (p.contains("total")) ev["total"] = p["total"];
                if (p.contains("digest")) ev["digest"] = p["digest"];
                emit(ev);
            });
            if (r.status != 200 || !err.empty()) throw std::runtime_error("download failed: " + (err.empty() ? "HTTP " + std::to_string(r.status) : err));
            emit({{"id", id}, {"event", "result"}, {"ok", true}});
        } else if (op == "delete") {
            const auto r = http("DELETE", "/api/delete", json{{"model", req.at("model")}}.dump());
            if (r.status != 200) throw std::runtime_error("delete failed: HTTP " + std::to_string(r.status));
            emit({{"id", id}, {"event", "result"}, {"ok", true}});
        } else {
            throw std::runtime_error("unknown op '" + op + "'");
        }
    } catch (const std::exception& e) {
        emit({{"id", id}, {"event", "error"}, {"message", e.what()}});
    }
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 2;
#endif
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
        std::cout << "anvil-ai 1\n";
        return 0;
    }
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        json req;
        try {
            req = json::parse(line);
        } catch (const std::exception& e) {
            emit({{"event", "error"}, {"message", std::string("bad request: ") + e.what()}});
            continue;
        }
        handle(req);
    }
    return 0;
}
