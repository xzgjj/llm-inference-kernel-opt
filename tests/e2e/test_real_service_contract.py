# test_real_service_contract.py — 真实服务契约冒烟（进程外，需已运行的真模型服务）
# 用法：设置 LLMOPT_REAL_BASE=http://127.0.0.1:18080 后执行；未设置时以 77 退出（CTest SKIP）。
# 与 scripts/bench/realpath_probe.py 的分工：本文件是可进 CTest 的最小契约集（快、稳定），
# 探针脚本负责 17 例全量探测与证据落盘，不要求进闸门。
from __future__ import annotations

import http.client
import json
import os
import sys

SKIP_RETURN_CODE = 77


def call(base: str, method: str, path: str, payload: dict | None = None) -> tuple[int, bytes]:
    host, port = base.replace("http://", "").split(":")
    conn = http.client.HTTPConnection(host, int(port), timeout=30)
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
    check(status == 200 and b"ready" in body, f"healthz 异常: {status} {body[:80]}")

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
    payload = json.loads(body)
    usage = payload.get("usage", {})
    check(status == 200 and usage.get("total_tokens") ==
          usage.get("prompt_tokens", 0) + usage.get("completion_tokens", 0),
          "非流式 usage 不自洽")

    status, body = call(base, "POST", "/completion", {"prompt": ""})
    check(status == 400 and json.loads(body)["error"]["code"] == "INVALID_JSON",
          f"空 prompt 语义错误: {status} {body[:80]}")

    status, body = call(base, "POST", "/completion", {"prompt": "hello", "max_tokens": 99999})
    message = json.loads(body)["error"]["message"]
    check(status == 400 and "max_tokens" in message, "越界错误未指出字段")

    status, body = call(base, "POST", "/completion", {"prompt": "token " * 1600})
    check(status == 400 and json.loads(body)["error"]["code"] == "CONTEXT_OVERFLOW",
          "超上下文未回 CONTEXT_OVERFLOW")

    if failures:
        for item in failures:
            print(f"FAIL: {item}")
        return 1
    print("PASS: 真实服务契约 8 项")
    return 0


if __name__ == "__main__":
    sys.exit(main())
