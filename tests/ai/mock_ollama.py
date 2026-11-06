"""A stand-in for a local Ollama server (same HTTP API subset) for tests.

Endpoints: GET /api/version, GET /api/tags, POST /api/pull (chunked NDJSON
progress), DELETE /api/delete, POST /api/chat (scripted tool calls chosen from
keywords of the last user message). Every chat request is written to
<log>/last_chat.json so tests can check what ANVIL sent.

usage: python mock_ollama.py PORT LOGDIR   (prints "READY" when listening)
"""
import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 11999
LOG = sys.argv[2] if len(sys.argv) > 2 else "."
INSTALLED = {"qwen2.5:7b": 4683087332}
LOCK = threading.Lock()


def scripted_calls(text, req):
    """Tool calls a model would plausibly emit for the test prompts."""
    t = text.lower()
    objs = [o for o in req["messages"][0]["content"].split("\n") if o.startswith("- ")]
    minis = [o for o in objs if "(Model" in o] or objs  # a model would pick the mini
    first = minis[0][2:].split(" ")[0] if minis else "hero"
    calls = []
    if "arm" in t:
        calls.append(("set_pose", {"object": first, "angles": {"r_shoulder_fwd": 90, "r_elbow": 30}}))
    if "attack" in t or "angriff" in t:
        calls.append(("pose_preset", {"object": first, "preset": "attack"}))
    if "sword" in t or "schwert" in t or "zweih" in t:
        calls.append(("equip_weapon", {"object": first, "weapon": "zweihander", "hand": "r", "pitch_deg": 20}))
    if "base" in t or "sockel" in t:
        calls.append(("add_base", {"diameter_mm": 32, "texture": "flagstone"}))
    if "flat" in t or "flach" in t:
        calls.append(("slice_flat", {"object": first, "z_mm": 0.4}))
    if "bogus" in t:
        calls.append(("set_pose", {"object": "NoSuchObject", "angles": {"r_elbow": 9999}}))
        calls.append(("launch_rockets", {"target": "moon"}))
    return calls


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):  # quiet
        pass

    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        n = int(self.headers.get("Content-Length", "0"))
        return json.loads(self.rfile.read(n) or b"{}")

    def do_GET(self):
        if self.path == "/api/version":
            return self._json(200, {"version": "0.12.0-mock"})
        if self.path == "/api/tags":
            with LOCK:
                models = [{"name": k, "model": k, "size": v, "digest": "sha256:" + "ab" * 32} for k, v in INSTALLED.items()]
            return self._json(200, {"models": models})
        self._json(404, {"error": "not found"})

    def do_DELETE(self):
        req = self._body()
        with LOCK:
            INSTALLED.pop(req.get("model"), None)
        self._json(200, {})

    def do_POST(self):
        req = self._body()
        if self.path == "/api/pull":
            name = req.get("model")
            self.send_response(200)
            self.send_header("Content-Type", "application/x-ndjson")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()

            def chunk(obj):
                data = (json.dumps(obj) + "\n").encode()
                self.wfile.write(b"%x\r\n%s\r\n" % (len(data), data))
                self.wfile.flush()
            if name == "missing:404":
                chunk({"error": "pull model manifest: file does not exist"})
            else:
                chunk({"status": "pulling manifest"})
                total = 1000
                for done in (0, 250, 500, 1000):
                    chunk({"status": "pulling abc", "digest": "sha256:abc", "total": total, "completed": done})
                chunk({"status": "verifying sha256 digest"})
                chunk({"status": "success"})
                with LOCK:
                    INSTALLED[name] = total
            self.wfile.write(b"0\r\n\r\n")
            return
        if self.path == "/api/chat":
            with open(os.path.join(LOG, "last_chat.json"), "w", encoding="utf-8") as f:
                json.dump(req, f, indent=1)
            if req.get("model") not in INSTALLED:
                return self._json(404, {"error": f"model '{req.get('model')}' not found, try pulling it first"})
            text = [m for m in req["messages"] if m["role"] == "user"][-1]["content"]
            calls = scripted_calls(text, req)
            msg = {"role": "assistant", "content": "" if calls else "Ich kann dabei helfen, Minis zu posen und auszurüsten."}
            if calls:
                msg["tool_calls"] = [{"function": {"name": n, "arguments": a}} for n, a in calls]
            return self._json(200, {"model": req["model"], "message": msg, "done": True})
        self._json(404, {"error": "not found"})


if __name__ == "__main__":
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), H)
    print("READY", flush=True)
    srv.serve_forever()
