# test_disconnect_cancel.py — S1 非流式断连取消：客户端断开后服务端 ≤2s 取消并释放 KV。
from __future__ import annotations

import json
import socket
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import server_kit as kit

DETECT_LIMIT_S = 2.0


def post_and_abort(port: int, max_tokens: int, hold_s: float) -> float:
    """发一条非流式请求，hold_s 后强制关闭连接；返回关闭时刻。"""
    body = json.dumps({"prompt": "disconnect-cancel", "max_tokens": max_tokens,
                       "temperature": 0}).encode()
    sock = socket.create_connection(("127.0.0.1", port), timeout=10)
    try:
        request = (b"POST /v1/completions HTTP/1.1\r\n"
                   b"Host: 127.0.0.1\r\nContent-Type: application/json\r\n"
                   b"Content-Length: " + str(len(body)).encode() + b"\r\n"
                   b"Connection: close\r\n\r\n" + body)
        sock.sendall(request)
        time.sleep(hold_s)
        closed_at = time.time()
        # RST 而不是 FIN：模拟浏览器/curl 被 Ctrl-C 后的硬断开。
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                        b"\x01\x00\x00\x00\x00\x00\x00\x00")
        return closed_at
    finally:
        sock.close()


def cancelled_total(port: int) -> float:
    status, body, _ = kit.get(port, "/metrics")
    assert status == 200, f"/metrics 应 200，实际 {status}"
    samples, _, _, _ = kit.parse_metrics(body.decode())
    return samples.get('llmopt_requests_total{status="cancelled"}', 0.0)


def wait_until(predicate, timeout: float, interval: float = 0.25) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return predicate()


def main() -> int:
    server = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as tmp:
        model = Path(tmp) / "fake.gguf"
        kit.make_gguf(model)
        env = {"LLMOPT_TEST_STUB_DELAY_MS": "50"}
        with kit.running_server(server, model,
                                ["--ctx-tokens", "1024", "--max-concurrency", "2",
                                 "--max-num-batched-tokens", "1024",
                                 "--log-level", "warn"], env_extra=env) as port:
            baseline = cancelled_total(port)
            closed_at = post_and_abort(port, max_tokens=600, hold_s=0.8)
            detected = wait_until(lambda: cancelled_total(port) > baseline, timeout=DETECT_LIMIT_S)
            latency = time.time() - closed_at
            assert detected, f"断连后 {DETECT_LIMIT_S}s 内未观察到 cancelled 计数 +1"
            assert latency <= DETECT_LIMIT_S + 0.5, f"取消延迟 {latency:.2f}s 超过上限"

            # KV 必须在 10s 内回零；running 必须归零。
            def kv_drained() -> bool:
                status, body, _ = kit.get(port, "/metrics")
                if status != 200:
                    return False
                samples, _, _, _ = kit.parse_metrics(body.decode())
                return (samples.get("llmopt_kv_used_blocks") == 0
                        and samples.get("llmopt_requests_running") == 0)

            assert wait_until(kv_drained, timeout=10.0), "断连取消后 KV / running 未在 10s 内归零"

            # 后续正常请求不受影响。
            status, body, _ = kit.post(port, "/v1/completions",
                                       {"prompt": "after-disconnect", "max_tokens": 8})
            assert status == 200, f"断连后正常请求应 200，实际 {status}"
            assert body.decode().lstrip().startswith("{"), "正常响应体应为 JSON"
            print(f"OK disconnect_cancel latency={latency:.2f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
