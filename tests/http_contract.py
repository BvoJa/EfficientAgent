#!/usr/bin/env python3
"""Exercise the real HTTP backend against a deterministic local fake server."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time

binary, repository = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
mode = "json"
requests = []

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def respond(self, payload, status=200):
        data = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        if self.path == "/props" and mode == "wrong_context":
            self.respond({"default_generation_settings":{"n_ctx":4096,"params":{}},"model_alias":"Qwen3-0.6B"})
            return
        self.respond({"status": "ok"})

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        requests.append(request)
        assert request["model"] == "Qwen3-0.6B"
        assert request["stream"] is False
        assert request["chat_template_kwargs"] == {"enable_thinking": False}
        assert len(request["response_format"]["json_schema"]["schema"]["oneOf"]) == (6 if len(requests) == 1 or mode == "malformed" else 7)
        branches = request["response_format"]["json_schema"]["schema"]["oneOf"]
        assert list(branches[0]["properties"]) == ["type", "name", "arguments"]
        if mode == "error":
            self.respond({"error": "intentional"}, 503)
            return
        if mode == "slow":
            time.sleep(0.25)
        first = len(requests) == 1
        action = {"type": "tool", "name": "inspect_csv", "arguments": {"path": "data.csv"}} if first else {
            "type": "final", "success": True, "answer": "Fixture inspected", "evidence": ["obs-1"]}
        message = {"content": json.dumps(action)}
        if mode == "native" and first:
            message = {"content": None, "tool_calls": [{"id": "call-1", "type": "function", "function": {
                "name": "inspect_csv", "arguments": '{"path":"data.csv"}'}}]}
        if mode == "malformed":
            message = {"content": "not JSON"}
        self.respond({"choices": [{"finish_reason": "stop", "message": message}],
                      "usage": {"prompt_tokens": 80, "completion_tokens": 12},
                      "timings": {"prompt_ms": 1.2, "predicted_ms": 2.4}})

server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    with tempfile.TemporaryDirectory(prefix="edge-http-") as temp:
        root = Path(temp)
        (root / "data.csv").write_text("name,value\na,1\n")
        config = json.loads((repository / "configs/agent.json").read_text())
        config["inference"]["port"] = server.server_port
        config["workspace"] = str(root)
        for mode in ("json", "native", "error", "malformed", "slow", "wrong_context"):
            requests.clear()
            config["agent"]["timeout_ms"] = 80 if mode == "slow" else 5000
            config_path = root / "config.json"
            config_path.write_text(json.dumps(config))
            process = subprocess.run([str(binary), "--config", str(config_path), "--task", "Inspect data.csv"], capture_output=True, text=True, timeout=10)
            result = json.loads(process.stdout)
            if mode in ("json", "native"):
                assert process.returncode == 0 and result["success"], result
                assert result["metrics"]["prompt_tokens"] == 160, result
                assert result["metrics"]["completion_tokens"] == 24, result
            else:
                assert process.returncode != 0, result
                if mode == "slow":
                    assert result["state"] == "timed_out", result
                if mode == "malformed":
                    assert result["metrics"]["invalid_outputs"] == 3, result
                if mode == "wrong_context":
                    assert result["error"]["code"] == "server_config", result
            print("PASS HTTP", mode)
finally:
    server.shutdown()
    server.server_close()
