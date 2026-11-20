# test_server_concurrency.py — 进程级连续批：4 路并发、同前缀复用、指标与对账零差异。
# 由 CTest 传入 llmopt_server 路径；stub 后端保证无 GPU/模型依赖。
from __future__ import annotations

import http.client
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

SERVER = Path(sys.argv[1])


def make_gguf(path: Path) -> None:
    """写最小 GGUF 头：2 层/4 头，让门控在 CPU 上直接放行。"""

    def string(text: str) -> bytes:
        return struct.pack("<Q", len(text.encode())) + text.encode()

    def kv_u32(key: str, value: int) -> bytes:
        return string(key) + struct.pack("<I", 4) + struct.pack("<I", value)

    body = struct.pack("<IIQQ", 0x46554747, 3, 0, 4)
    body += kv_u32("llama.block_count", 2)
    body += kv_u32("llama.attention.head_count", 4)
    body += kv_u32("llama.attention.head_count_kv", 2)
    body += kv_u32("llama.embedding_length", 128)
    path.write_bytes(body)


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def get(port: int, path: str) -> tuple[int, bytes]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
    try:
        conn.request("GET", path)
        response = conn.getresponse()
        return response.status, response.read()
    finally:
        conn.close()


def post(port: int, path: str, payload: dict) -> tuple[int, bytes]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    try:
        conn.request("POST", path, json.dumps(payload), {"Content-Type": "application/json"})
        response = conn.getresponse()
        return response.status, response.read()
    finally:
        conn.close()


def wait_ready(port: int, proc: subprocess.Popen[bytes], timeout: float = 20.0) -> None:
    """服务没起来就带上进程输出失败，避免 30s 超时后无线索。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            out, err = proc.communicate()
            raise RuntimeError(f"服务提前退出 rc={proc.returncode}\n{out}\n{err}")
        try:
            if get(port, "/healthz")[0] == 200:
                return
        except OSError:
            pass
        time.sleep(0.2)
    raise RuntimeError("等待 /healthz 就绪超时")


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def stream_text(port: int, prompt: str, max_tokens: int = 4) -> list[str]:
    """发一次 SSE 请求，返回每帧文本，便于并发线程各自校验。"""
    status, body = post(port, "/completion",
                        {"prompt": prompt, "stream": True,
                         "max_tokens": max_tokens, "seed": 42})
    check(status == 200, f"流式请求应 200，实际 {status}")
    lines = [line for line in body.decode().splitlines() if line.startswith("data: ")]
    check(lines and lines[-1] == "data: [DONE]", "SSE 缺少 [DONE]")
    frames = [json.loads(line[len("data: "):]) for line in lines[:-1]]
    return [frame["choices"][0]["text"] for frame in frames]


def metric(body: bytes, name: str) -> int:
    """从 Prometheus 文本里取一条指标值，缺省为 0。"""
    for line in body.decode().splitlines():
        if line.startswith(name + " "):
            return int(line.rsplit(" ", 1)[1])
    return 0


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        model = Path(tmp) / "fake.gguf"
        make_gguf(model)
        port = free_port()
        env = dict(os.environ)
        env["LLMOPT_ALLOW_STUB"] = "1"
        cmd = [str(SERVER), "--model", str(model), "--host", "127.0.0.1", "--port", str(port),
               "--backend", "stub", "--mode", "off", "--ctx-tokens", "512",
               "--max-concurrency", "4", "--max-num-batched-tokens", "512",
               "--block-size", "8", "--queue-capacity", "8", "--log-level", "warn"]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        try:
            wait_ready(port, proc)
            prompts = [f"concurrent-prompt-{index:02d}" for index in range(4)]
            results: dict[str, list[str]] = {}
            errors: list[Exception] = []

            def worker(index: int) -> None:
                try:
                    results[prompts[index]] = stream_text(port, prompts[index])
                except Exception as exc:  # noqa: BLE001 — 线程内错误汇总后统一失败
                    errors.append(exc)

            threads = [threading.Thread(target=worker, args=(index,)) for index in range(4)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            check(not errors, f"并发请求出现异常: {errors}")
            check(len(results) == 4, "4 路并发应全部完成")
            check(all(len(frames) == 4 for frames in results.values()), "每路应产出 4 帧")

            # 同前缀第二次请求：19 字符 = 2 个 8-token 满块，应命中 16 token。
            long_prompt = "prefix-cache-prompt"
            first = stream_text(port, long_prompt)
            second = stream_text(port, long_prompt)
            check(first == second, "同前缀二次请求输出应逐帧一致")

            status, body = get(port, "/metrics")
            check(status == 200, "metrics 应 200")
            check(metric(body, "llmopt_prefix_hits_total") >= 1, "同前缀应命中块缓存")
            check(metric(body, "llmopt_prefix_hit_tokens_total") >= 16, "命中应覆盖 16 token")
            check(metric(body, "llmopt_kv_reconcile_diffs_total") == 0, "KV 位置对账应零差异")
            check(metric(body, "llmopt_requests_total{status=\"ok\"}") == 6,
                  "6 条请求应全部按 ok 收尾")
            check(metric(body, "llmopt_kv_free_blocks") > 0, "donor 存在时仍应有空闲块")
            print("OK integration_server_concurrency")
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
