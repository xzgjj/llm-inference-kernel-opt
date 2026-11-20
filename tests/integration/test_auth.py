# test_auth.py — D6-2：配置 api-key 后 /v1/** 必须带 Bearer，探针与指标保持开放。
from __future__ import annotations

import http.client
import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import server_kit as kit  # noqa: E402


def post_auth(port: int, path: str, payload: dict, authorization: str = "") -> tuple[int, dict, dict]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=20)
    headers = {"Content-Type": "application/json"}
    if authorization:
        headers["Authorization"] = authorization
    try:
        conn.request("POST", path, json.dumps(payload), headers)
        response = conn.getresponse()
        return response.status, json.loads(response.read()), dict(response.getheaders())
    finally:
        conn.close()


def main() -> int:
    server = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as tmp:
        model = Path(tmp) / "auth.gguf"
        kit.make_gguf(model)
        with kit.running_server(server, model, ["--api-key", "secret-token"]) as port:
            payload = {"prompt": "hello", "max_tokens": 2}
            status, body, headers = post_auth(port, "/v1/completions", payload)
            assert status == 401, (status, body)
            assert body["error"]["code"] == "UNAUTHORIZED", body
            assert headers.get("WWW-Authenticate", "").startswith("Bearer"), headers

            status, body, _ = post_auth(port, "/v1/completions", payload, "Bearer wrong")
            assert status == 401, (status, body)
            assert body["error"]["code"] == "UNAUTHORIZED", body

            status, body, _ = post_auth(port, "/v1/chat/completions",
                                        {"messages": [{"role": "user", "content": "hi"}],
                                         "max_tokens": 2}, "Bearer secret-token")
            assert status == 200, (status, body)

            status, body, _ = kit.get(port, "/healthz")
            assert status == 200
            status, body, _ = kit.get(port, "/readyz")
            assert status == 200
            status, body, _ = kit.get(port, "/metrics")
            assert status == 200
            status, body, _ = kit.get(port, "/v1/models")
            assert status == 401, status
    print("auth ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
