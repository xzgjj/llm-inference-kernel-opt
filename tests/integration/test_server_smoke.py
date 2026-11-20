# test_server_smoke.py — 进程级冒烟：stub 后端 + 合成 GGUF，校验路由/SSE/非流式/404。
# 由 CTest 传入 llmopt_server 可执行路径；不依赖真实模型或 GPU。
from __future__ import annotations

import http.client
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SERVER = Path(sys.argv[1])
EVIDENCE_DIR = Path(sys.argv[2]) if len(sys.argv) > 2 else None


def make_gguf(path: Path, block_count: int = 2, head_count: int = 4,
              head_count_kv: int = 2, embedding_length: int = 128) -> None:
    """写最小 GGUF 头；默认 2 层/4 头，超大档用于触发门控拒绝。"""

    def string(text: str) -> bytes:
        return struct.pack("<Q", len(text.encode())) + text.encode()

    def kv_u32(key: str, value: int) -> bytes:
        return string(key) + struct.pack("<I", 4) + struct.pack("<I", value)

    body = struct.pack("<IIQQ", 0x46554747, 3, 0, 4)
    body += kv_u32("llama.block_count", block_count)
    body += kv_u32("llama.attention.head_count", head_count)
    body += kv_u32("llama.attention.head_count_kv", head_count_kv)
    body += kv_u32("llama.embedding_length", embedding_length)
    path.write_bytes(body)


def free_port() -> int:
    """让内核挑一个空闲端口，避免 CI 并行撞车。"""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def post(port: int, path: str, payload: dict) -> tuple[int, bytes]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
    try:
        conn.request("POST", path, json.dumps(payload), {"Content-Type": "application/json"})
        response = conn.getresponse()
        return response.status, response.read()
    finally:
        conn.close()


def get(port: int, path: str) -> tuple[int, bytes]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
    try:
        conn.request("GET", path)
        response = conn.getresponse()
        return response.status, response.read()
    finally:
        conn.close()


def wait_ready(port: int, proc: subprocess.Popen[bytes], timeout: float = 20.0) -> None:
    """轮询 /healthz；进程提前退出则直接失败并带上输出。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            out, err = proc.communicate()
            raise RuntimeError(f"服务提前退出 rc={proc.returncode}\n{out}\n{err}")
        try:
            status, _ = get(port, "/healthz")
            if status == 200:
                return
        except OSError:
            pass
        time.sleep(0.2)
    raise RuntimeError("等待 /healthz 就绪超时")


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run_checks(port: int) -> None:
    status, _ = get(port, "/healthz")
    check(status == 200, "healthz 应为 200")

    status, body = get(port, "/metrics")
    check(status == 200 and b"llmopt_requests_total" in body, "metrics 缺少请求计数")

    status, body = post(port, "/completion",
                        {"prompt": "hello", "stream": True, "max_tokens": 3, "seed": 42})
    check(status == 200, f"流式请求应 200，实际 {status}")
    lines = [line for line in body.decode().splitlines() if line.startswith("data: ")]
    check(len(lines) >= 4, f"应至少有 3 帧 + [DONE]，实际 {len(lines)}")
    check(lines[-1] == "data: [DONE]", "最后一帧必须是 [DONE]")
    frames = [json.loads(line[len("data: "):]) for line in lines[:-1]]
    check(all(frame["choices"] for frame in frames), "帧缺少 choices")
    check(frames[-1]["choices"][0]["finish_reason"] == "length", "末帧结束原因应为 length")
    check(all(len(frame["choices"][0]["text"]) == 1 for frame in frames), "每帧应产出一个字符")

    status, body = post(port, "/v1/completions",
                        {"prompt": "hello", "stream": False, "max_tokens": 2})
    check(status == 200, f"非流式请求应 200，实际 {status}")
    payload = json.loads(body)
    check(len(payload["choices"][0]["text"]) == 2, "非流式应返回 2 个字符")
    check(payload["usage"]["total_tokens"] == payload["usage"]["prompt_tokens"] + 2,
          "usage.total 应等于 prompt + completion")

    status, body = post(port, "/v1/chat/completions", {"messages": []})
    check(status == 400, f"空 messages 应 400，实际 {status}")
    check(json.loads(body)["error"]["code"] == "INVALID_JSON", "空 messages 应回 INVALID_JSON")

    status, body = post(port, "/completion", {"prompt": ""})
    check(status == 400, f"空 prompt 应 400，实际 {status}")
    # 空 prompt 是语义错误而非上下文超限，避免客户端据此裁剪 prompt。
    check(json.loads(body)["error"]["code"] == "INVALID_JSON", "空 prompt 应回 INVALID_JSON")

    status, body = post(port, "/completion", {"prompt": "hello", "max_tokens": 5000})
    check(status == 400, f"max_tokens 越界应 400，实际 {status}")
    check("max_tokens" in json.loads(body)["error"]["message"], "错误信息应指出 max_tokens 字段")


def stream_tokens(port: int, max_tokens: int = 8) -> list[dict]:
    """发一次 SSE 请求，按帧取出 token_id/text/finish_reason。"""
    status, body = post(port, "/completion",
                        {"prompt": "determinism", "stream": True,
                         "max_tokens": max_tokens, "seed": 42})
    check(status == 200, f"确定性请求应 200，实际 {status}")
    lines = [line for line in body.decode().splitlines() if line.startswith("data: ")]
    check(lines and lines[-1] == "data: [DONE]", "确定性请求缺少 [DONE]")
    frames = [json.loads(line[len("data: "):]) for line in lines[:-1]]
    return [{"token_id": frame["choices"][0]["token_id"],
             "text": frame["choices"][0]["text"],
             "finish_reason": frame["choices"][0]["finish_reason"]} for frame in frames]


def write_evidence(first: list[dict], second: list[dict]) -> None:
    """落盘阶段 1 出口证据：逐 token 两轮对照与门控画像。"""
    if EVIDENCE_DIR is None:
        return
    import csv

    EVIDENCE_DIR.mkdir(parents=True, exist_ok=True)
    with (EVIDENCE_DIR / "single_request.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["run", "index", "token_id", "text", "finish_reason", "same_as_run1"])
        for run_name, rows in (("run1", first), ("run2", second)):
            for index, row in enumerate(rows):
                same = index < len(first) and row["token_id"] == first[index]["token_id"]
                writer.writerow([run_name, index, row["token_id"], row["text"],
                                 row["finish_reason"] or "", int(same)])


def probe_gate_profile(model: Path, ctx_tokens: int, label: str) -> dict:
    """起一次进程读门控结论；拒绝档应在 listen 前退出。"""
    env = dict(os.environ)
    env["LLMOPT_ALLOW_STUB"] = "1"
    cmd = [str(SERVER), "--model", str(model), "--host", "127.0.0.1",
           "--port", str(free_port()), "--backend", "stub", "--mode", "off",
           "--ctx-tokens", str(ctx_tokens), "--log-level", "warn"]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=30, env=env)
    output = proc.stdout + proc.stderr
    decision = "BLOCKED" if "decision=BLOCKED" in output else "ELIGIBLE"
    return {"label": label, "ctx_tokens": ctx_tokens, "exit_code": proc.returncode,
            "decision": decision}



def main() -> int:
    gate_profiles: list[dict] = []
    with tempfile.TemporaryDirectory() as tmp:
        model = Path(tmp) / "fake.gguf"
        make_gguf(model)
        port = free_port()
        env = dict(os.environ)
        env["LLMOPT_ALLOW_STUB"] = "1"
        cmd = [str(SERVER), "--model", str(model), "--host", "127.0.0.1", "--port", str(port),
               "--backend", "stub", "--mode", "off", "--ctx-tokens", "512",
               "--max-concurrency", "1", "--log-level", "warn"]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        try:
            wait_ready(port, proc)
            run_checks(port)
            first = stream_tokens(port)
            second = stream_tokens(port)
            check([row["token_id"] for row in first] == [row["token_id"] for row in second],
                  "同种子两次请求的 token 序列应完全一致")
            write_evidence(first, second)
            gate_profiles.append({"label": "normal", "ctx_tokens": 512, "exit_code": 0,
                                  "decision": "ELIGIBLE", "note": "healthz ready"})
            print("OK integration_server_smoke")
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)

        big_model = Path(tmp) / "fake-big-kv.gguf"
        make_gguf(big_model, block_count=128, head_count=1, head_count_kv=64,
                  embedding_length=32768)
        blocked = probe_gate_profile(big_model, 512, "oversized_kv")
        check(blocked["exit_code"] == 3, f"超预算应退出 3，实际 {blocked['exit_code']}")
        check(blocked["decision"] == "BLOCKED", "超预算决策应为 BLOCKED")
        gate_profiles.append(blocked)

    if EVIDENCE_DIR is not None:
        EVIDENCE_DIR.mkdir(parents=True, exist_ok=True)
        (EVIDENCE_DIR / "gate_profiles.json").write_text(
            json.dumps(gate_profiles, ensure_ascii=False, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
