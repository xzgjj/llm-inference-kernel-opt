# test_real_service_contract.py — 真实服务契约冒烟（进程外，需已运行的真模型服务）
# 用法：设置 LLMOPT_REAL_BASE=http://127.0.0.1:18080（只写 scheme://host:port，不带路径）
# 后执行；未设置时以 77 退出（CTest SKIP）。健康口径固定为 GET /healthz + GET /readyz。
# 与 scripts/bench/realpath_probe.py 的分工：本文件是可进 CTest 的最小契约集（快、稳定），
# 探针脚本负责 17 例全量探测与证据落盘，不要求进闸门。
from __future__ import annotations

import http.client
import json
import os
import sys
import urllib.parse

SKIP_RETURN_CODE = 77


def normalize_base(raw: str) -> tuple[str, int]:
    """只接受根地址；带路径的 base 视为配置错误，避免端点被拼成 /v1/healthz。"""
    parsed = urllib.parse.urlparse(raw)
    if parsed.scheme not in ("http", "https") or not parsed.hostname:
        raise ValueError(f"LLMOPT_REAL_BASE 非法: {raw}")
    if parsed.path not in ("", "/"):
        raise ValueError(f"LLMOPT_REAL_BASE 只写到端口，不要带路径: {raw}")
    return parsed.hostname, parsed.port or (443 if parsed.scheme == "https" else 80)


def parse_json_body(body: bytes) -> dict:
    """容错解析响应体；非 JSON / 非对象一律返回空 dict，避免用例硬崩。"""
    try:
        data = json.loads(body)
        return data if isinstance(data, dict) else {}
    except (json.JSONDecodeError, UnicodeDecodeError, ValueError):
        return {}


def error_object(body: bytes) -> dict:
    """取统一错误体里的 error 对象；缺失或类型不符时返回空 dict。"""
    err = parse_json_body(body).get("error")
    return err if isinstance(err, dict) else {}


def call(base: str, method: str, path: str, payload: dict | None = None) -> tuple[int, bytes]:
    host, port = normalize_base(base)
    conn = http.client.HTTPConnection(host, port, timeout=30)
    try:
        body = json.dumps(payload) if payload is not None else None
        headers = {"Content-Type": "application/json"} if body else {}
        conn.request(method, path, body, headers)
        response = conn.getresponse()
        return response.status, response.read()
    finally:
        conn.close()


def main() -> int:
    base = os.environ.get("LLMOPT_REAL_BASE", "")
    if not base:
        print("SKIP: 未设置 LLMOPT_REAL_BASE，真实服务契约用例不执行")
        return SKIP_RETURN_CODE

    failures: list[str] = []

    def check(cond: bool, message: str) -> None:
        if not cond:
            failures.append(message)

    status, body = call(base, "GET", "/healthz")
    health = json.loads(body) if status == 200 and body else {}
    check(status == 200 and health.get("status") == "ready",
          f"healthz 异常: {status} {body[:80]}")

    status, body = call(base, "GET", "/readyz")
    ready = json.loads(body) if status == 200 and body else {}
    check(status == 200 and ready.get("status") == "ready",
          f"readyz 异常: {status} {body[:80]}")

    status, body = call(base, "GET", "/metrics")
    check(status == 200 and body.count(b"\n") >= 40, f"metrics series 过少: {status}")

    status, body = call(base, "POST", "/completion",
                        {"prompt": "hello", "stream": True, "max_tokens": 4, "seed": 42})
    check(status == 200, f"SSE 应 200，实际 {status}")
    lines = [line for line in body.decode().splitlines() if line.startswith("data: ")]
    check(bool(lines and lines[-1] == "data: [DONE]"), "SSE 未以 [DONE] 结束")
    first = json.loads(lines[0][6:]) if lines else {}
    check(bool(first.get("choices")), "SSE 帧缺 choices")

    status, body = call(base, "POST", "/completion",
                        {"prompt": "hello", "stream": False, "max_tokens": 3})
    payload = parse_json_body(body)
    usage = payload.get("usage", {}) if isinstance(payload.get("usage"), dict) else {}
    check(status == 200 and usage.get("total_tokens") ==
          usage.get("prompt_tokens", 0) + usage.get("completion_tokens", 0),
          "非流式 usage 不自洽")

    status, body = call(base, "POST", "/completion", {"prompt": ""})
    check(status == 400 and error_object(body).get("code") == "INVALID_JSON",
          f"空 prompt 语义错误: {status} {body[:80]}")

    status, body = call(base, "POST", "/completion", {"prompt": "hello", "max_tokens": 99999})
    message = error_object(body).get("message", "")
    check(status == 400 and "max_tokens" in message, "越界错误未指出字段")

    # 超上下文：默认 1600 个 "token " 单词，配合脚本的 --ctx-tokens 1024；
    # 换更大的 ctx 时用 LLMOPT_E2E_OVERFLOW_TOKENS 调整，避免用例语义漂移。
    try:
        overflow_words = int(os.environ.get("LLMOPT_E2E_OVERFLOW_TOKENS", "1600"))
    except ValueError:
        overflow_words = 1600
        print("WARN: LLMOPT_E2E_OVERFLOW_TOKENS 非整数，回退 1600")
    status, body = call(base, "POST", "/completion", {"prompt": "token " * overflow_words})
    check(status == 400 and error_object(body).get("code") == "CONTEXT_OVERFLOW",
          f"超上下文未回 CONTEXT_OVERFLOW: {status} {body[:80]}")

    if failures:
        for item in failures:
            print(f"FAIL: {item}")
        return 1
    print("PASS: 真实服务契约 10 项"
          "（healthz/readyz/metrics/SSE-200/[DONE]/choices/非流式 usage/"
          "空 prompt/越界字段/超上下文）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
