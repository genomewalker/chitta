"""Run the real C++ HTTP client against a local, deterministic Ollama stub."""
import http.server
import json
import subprocess
import sys
import threading


def main():
    requests = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            requests.append((self.path, body))
            message = {"content": "[PATTERN] a→b", "thinking": "must not reach parser"}
            response = {"message": message} if self.path == "/api/chat" else {"choices": [{"message": message}]}
            data = json.dumps(response).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    with http.server.HTTPServer(("127.0.0.1", 0), Handler) as server:
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            subprocess.run([sys.argv[1], f"http://127.0.0.1:{server.server_port}"], check=True)
        finally:
            server.shutdown()
            thread.join()
    assert len(requests) == 3
    for (path, body), think in zip(requests[:2], (True, False), strict=True):
        assert path == "/api/chat" and body["think"] is think and body["stream"] is False
        assert body["options"]["num_predict"] == 8192
        assert abs(body["options"]["temperature"] - 0.3) < 1e-6
        assert body["messages"] == [{"role": "system", "content": "system"}, {"role": "user", "content": "body"}]
    assert requests[2][0] == "/v1/chat/completions"
    assert "think" not in requests[2][1] and requests[2][1]["max_tokens"] == 8192
    print("HTTP wire: think=true/false, options, content extraction, legacy compatibility PASS")


if __name__ == "__main__":
    main()
