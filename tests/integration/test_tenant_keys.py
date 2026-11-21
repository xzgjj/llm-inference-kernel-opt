# test_tenant_keys.py — 阶段 11：静态 key 文件鉴权、每 key 限流与 KV 配额。
from __future__ import annotations

import hashlib
import http.client
import json
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import server_kit as kit  # noqa: E402

KEY_A = "lk_" + "a" * 32
KEY_B = "lk_" + "b" * 32
KEY_C = "lk_" + "c" * 32


def sha256(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()


# DEBUG-11：/v1/models 与推理端点共用鉴权；只配 key 文件时也必须带合法 key。
def get_models(port: int, key: str = "") -> tuple[int, dict, dict]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    headers = {}
    if key:
        headers["Authorization"] = f"Bearer {key}"
    try:
        conn.request("GET", "/v1/models", headers=headers)
        response = conn.getresponse()
        body = json.loads(response.read().decode())
        return response.status, body, dict(response.getheaders())
    finally:
        conn.close()


def post(port: int, path: str, payload: dict, key: str = "") -> tuple[int, dict, dict]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    headers = {"Content-Type": "application/json"}
    if key:
        headers["Authorization"] = f"Bearer {key}"
    try:
        conn.request("POST", path, json.dumps(payload), headers)
        response = conn.getresponse()
        body = json.loads(response.read())
        return response.status, body, dict(response.getheaders())
    finally:
        conn.close()


def post_stream(port: int, payload: dict, key: str) -> tuple[int, str]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    headers = {"Content-Type": "application/json", "Authorization": f"Bearer {key}"}
    try:
        conn.request("POST", "/v1/completions", json.dumps(payload), headers)
        response = conn.getresponse()
        return response.status, response.read().decode()
    finally:
        conn.close()


def write_keys(path: Path) -> None:
    path.write_text(
        json.dumps(
            {
                "version": 1,
                "keys": [
                    {
                        "id": "tenant-a",
                        "sha256": sha256(KEY_A),
                        "quota": {
                            "max_concurrency": 1,
                            "queue_capacity": 1,
                            "rate_limit_rps": 0.5,
                            "rate_limit_burst": 1,
                            "kv_blocks": 0,
                        },
                    },
                    {
                        "id": "tenant-b",
                        "sha256": sha256(KEY_B),
                        "quota": {"max_concurrency": 2, "queue_capacity": 2},
                    },
                    {
                        "id": "tenant-c",
                        "sha256": sha256(KEY_C),
                        "quota": {"max_concurrency": 1, "queue_capacity": 0, "kv_blocks": 1},
                    },
                ],
            }
        ),
        encoding="utf-8",
    )
    os.chmod(path, 0o600)


def main() -> int:
    server = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as tmp:
        model = Path(tmp) / "tenant.gguf"
        kit.make_gguf(model)
        keys = Path(tmp) / "keys.json"
        write_keys(keys)
        args = ["--api-keys-file", str(keys), "--max-concurrency", "4",
                "--queue-capacity", "8", "--block-size", "4",
                "--max-num-batched-tokens", "512", "--chunk-size", "32"]
        with kit.running_server(server, model, args) as port:
            payload = {"prompt": "hello", "max_tokens": 2}

            status, body, headers = post(port, "/v1/completions", payload)
            assert status == 401, (status, body)
            assert body["error"]["code"] == "UNAUTHORIZED", body
            assert headers.get("WWW-Authenticate", "").startswith("Bearer"), headers

            status, body, _ = post(port, "/v1/completions", payload, "who")
            assert status == 401, (status, body)

            status, body, _ = get_models(port)
            assert status == 401, (status, body)
            assert body["error"]["code"] == "UNAUTHORIZED", body
            status, body, _ = get_models(port, KEY_A)
            assert status == 200, (status, body)
            assert body["data"], body

            status, body, _ = post(port, "/v1/completions", payload, KEY_A)
            assert status == 200, (status, body)

            status, body, _ = post(port, "/v1/completions", payload, KEY_A)
            assert status == 429, (status, body)
            assert body["error"]["code"] == "RATE_LIMITED", body

            status, body, _ = post(port, "/v1/completions", payload, KEY_B)
            assert status == 200, (status, body)

            status, stream_body = post_stream(
                port, {"prompt": "stream", "max_tokens": 3, "stream": True}, KEY_B)
            assert status == 200, status
            assert "data: [DONE]" in stream_body, stream_body

            status, body, _ = post(port, "/v1/completions",
                                   {"prompt": "x" * 40, "max_tokens": 4},
                                   KEY_C)
            assert status == 429, (status, body)
            assert body["error"]["code"] == "QUOTA_EXCEEDED", body

            status, metrics_text, _ = kit.get(port, "/metrics")
            assert status == 200
            metrics_text = metrics_text.decode()
            assert 'llmopt_tenant_requests_total{tenant="tenant-a",status="ok"} 1' in metrics_text
            assert 'llmopt_tenant_requests_total{tenant="tenant-b",status="ok"} 2' in metrics_text
            assert ('llmopt_tenant_quota_rejected_total{tenant="tenant-a",reason="rate_limit"} 1'
                    in metrics_text)
            assert ('llmopt_tenant_quota_rejected_total{tenant="tenant-c",reason="quota"} 1'
                    in metrics_text)
            assert 'llmopt_tenant_kv_used_blocks{tenant="tenant-c"}' in metrics_text
            assert 'tenant="tenant-z"' not in metrics_text
    print("tenant keys ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
