"""test_tenant_quota.py — D2-12：token 预算按租户隔离，超限 429 + Retry-After。"""
from __future__ import annotations

import hashlib
import http.client
import json
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import server_kit as kit

KEY_A = "lk_" + "a" * 32
KEY_B = "lk_" + "b" * 32


def sha256(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()


def write_keys(path: Path) -> None:
    payload = {"version": 1, "keys": [
        {"id": "tenant-a", "sha256": sha256(KEY_A),
         "quota": {"max_concurrency": 1, "queue_capacity": 2,
                   "tokens_per_hour": 4, "tokens_per_day": 6}},
        {"id": "tenant-b", "sha256": sha256(KEY_B),
         "quota": {"max_concurrency": 1, "queue_capacity": 2}},
    ]}
    path.write_text(json.dumps(payload), encoding="utf-8")
    os.chmod(path, 0o600)


def post(port: int, key: str, payload: dict) -> tuple[int, dict, bytes]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    headers = {"Content-Type": "application/json", "Authorization": f"Bearer {key}"}
    try:
        conn.request("POST", "/v1/completions", json.dumps(payload), headers)
        response = conn.getresponse()
        return response.status, dict(response.getheaders()), response.read()
    finally:
        conn.close()


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    server = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        model = root / "fake.gguf"
        kit.make_gguf(model)
        keys = root / "keys.json"
        write_keys(keys)
        with kit.running_server(server, model,
                               ["--ctx-tokens", "512", "--max-concurrency", "2",
                                "--max-num-batched-tokens", "256", "--log-level", "warn",
                                "--api-keys-file", str(keys)]) as port:
            # 第一次：tenant-a 用掉 4 个 completion token，正好打满小时预算。
            status, _, body = post(port, KEY_A, {"prompt": "a-one", "max_tokens": 4, "seed": 1})
            check(status == 200, f"tenant-a 首个请求应 200，实际 {status}: {body[:120]}")

            # 第二次：同一租户被 429 拒绝，且必须带 Retry-After。
            status, headers, body = post(port, KEY_A,
                                        {"prompt": "a-two", "max_tokens": 2, "seed": 2})
            check(status == 429, f"tenant-a 超预算应 429，实际 {status}: {body[:120]}")
            check("Retry-After" in headers, f"429 必须带 Retry-After，实际头 {headers}")
            retry = int(headers["Retry-After"])
            check(0 < retry <= 3600, f"Retry-After 应落在 1..3600，实际 {retry}")
            payload = json.loads(body)
            error = payload.get("error", payload)
            check(error.get("code") == "QUOTA_EXCEEDED",
                  f"错误码应为 QUOTA_EXCEEDED，实际 {error}")
            # 脱敏：错误体不能回显 token 或 key。
            check(KEY_A not in body.decode(), "响应不应回显 key")

            # 邻居租户不受影响：tenant-b 无 token 预算，照常 200。
            status, _, body = post(port, KEY_B, {"prompt": "b-one", "max_tokens": 4, "seed": 3})
            check(status == 200, f"tenant-b 应 200，实际 {status}: {body[:120]}")

            # 指标面：按 tenant/window 两个维度暴露已用 token。
            status, metrics, _ = kit.get(port, "/metrics")
            check(status == 200, "/metrics 应 200")
            text = metrics.decode()
            check('llmopt_tenant_tokens_used{tenant="tenant-a",window="hour"} 4' in text,
                  "tenant-a 小时窗口计数应为 4")
            check('llmopt_tenant_tokens_used{tenant="tenant-b",window="hour"} 4' in text,
                  "tenant-b 小时窗口计数应为 4")
    print("OK integration_tenant_quota")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
