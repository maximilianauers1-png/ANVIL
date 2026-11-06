"""anvil-ai worker protocol against the stand-in Ollama server.

usage: python test_worker.py <anvil-ai executable>   (prints "AI WORKER OK")
"""
import json
import os
import socket
import subprocess
import sys
import tempfile

worker = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
tmp = tempfile.mkdtemp(prefix="anvil_ai_")
with socket.socket() as s:
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
mock = subprocess.Popen([sys.executable, os.path.join(here, "mock_ollama.py"), str(port), tmp], stdout=subprocess.PIPE, text=True)
assert mock.stdout.readline().strip() == "READY"


def run(reqs, url):
    env = dict(os.environ, ANVIL_OLLAMA_URL=url)
    env.pop("OLLAMA_HOST", None)
    p = subprocess.run([worker], input="\n".join(json.dumps(r) for r in reqs) + "\n", capture_output=True, text=True,
                       env=env, timeout=60)
    return [json.loads(l) for l in p.stdout.splitlines() if l.strip()]


def ok(c, m):
    if not c:
        mock.kill()
        raise AssertionError(m)
    print("  ok:", m)


try:
    url = f"http://127.0.0.1:{port}"
    out = run([{"id": 1, "op": "status"}], url)
    ok(out[0]["running"] and out[0]["version"] == "0.12.0-mock", "status: server and version")
    out = run([{"id": 2, "op": "pull", "model": "qwen3:8b"}], url)
    prog = [o for o in out if o["event"] == "progress"]
    ok(any(o.get("completed") == 1000 for o in prog) and out[-1] == {"id": 2, "event": "result", "ok": True},
       f"pull streams progress ({len(prog)} events, chunked) and finishes")
    out = run([{"id": 3, "op": "pull", "model": "missing:404"}], url)
    ok(out[-1]["event"] == "error" and "does not exist" in out[-1]["message"], "pull error is reported")
    out = run([{"id": 4, "op": "chat", "request": {"model": "qwen2.5:7b", "messages": [
        {"role": "system", "content": "Scene objects:\n- hero (Model)"}, {"role": "user", "content": "attack pose"}]}}], url)
    calls = out[0]["response"]["message"]["tool_calls"]
    ok(calls[0]["function"]["name"] == "pose_preset", "chat returns tool calls")
    ok(json.load(open(os.path.join(tmp, "last_chat.json")))["stream"] is False, "chat is sent non-streaming")
    out = run([{"id": 5, "op": "delete", "model": "qwen3:8b"}, {"id": 6, "op": "status"}], url)
    ok(all(m["name"] != "qwen3:8b" for m in out[1]["models"]), "delete removes a model")
    for bad in ("http://10.1.2.3:11434", "http://example.com:11434", "https://127.0.0.1:11434"):
        out = run([{"id": 7, "op": "status"}], bad)
        ok(out[0]["running"] is False and ("refusing" in out[0]["message"] or "https" in out[0]["message"]),
           f"non-loopback server refused: {bad}")
    out = run([{"id": 8, "op": "status"}], "http://127.0.0.1:1")
    ok(out[0]["running"] is False and "no local AI server" in out[0]["message"], "no server: clear message")
finally:
    mock.kill()
print("AI WORKER OK")
