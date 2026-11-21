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


def raw_request(port: int, method: str, path: str, body: bytes = b"",
                headers: dict | None = None) -> tuple[int, bytes, dict]:
    """自定义方法/原始 body：用于 413 超限与 404/405 兜底报文回归。"""
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
    try:
        conn.request(method, path, body, headers or {"Content-Type": "application/json"})
        response = conn.getresponse()
        return response.status, response.read(), dict(response.getheaders())
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

    # R2-02：路径未登记 → 404 统一错误 JSON；方法不匹配 → 405 + Allow 头。
    status, body, _ = raw_request(port, "POST", "/nope")
    check(status == 404, f"未登记路径应 404，实际 {status}")
    err = json.loads(body)["error"]
    check(err["code"] == "NOT_SUPPORTED", f"未登记路径错误码应为 NOT_SUPPORTED，实际 {err}")

    status, body, headers = raw_request(port, "GET", "/v1/completions")
    check(status == 405, f"方法不匹配应 405，实际 {status}")
    err = json.loads(body)["error"]
    check(err["code"] == "NOT_SUPPORTED", f"方法不匹配错误码应为 NOT_SUPPORTED，实际 {err}")
    check("POST" in headers.get("Allow", ""), f"405 应带 Allow: POST，实际 {headers}")

    # R2-01：超 4MB body → 413 统一错误 JSON，body 可解析且含 code/retryable。
    big = json.dumps({"prompt": "hi", "pad": "x" * (5 * 1024 * 1024)}).encode()
    status, body, _ = raw_request(port, "POST", "/v1/completions", big)
    check(status == 413, f"超限 body 应 413，实际 {status}")
    err = json.loads(body)["error"]
    check(err["code"] == "PAYLOAD_TOO_LARGE", f"超限错误码应为 PAYLOAD_TOO_LARGE，实际 {err}")
    check("retryable" in err, "错误体应含 retryable 字段")


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


def write_profile(path: Path, file_bytes: int, block_count: int, head_count_kv: int,
                  name: str) -> None:
    """写容量档位 JSON（read_model_profile 声明字段，不需要真实权重）。"""
    path.write_text(json.dumps({
        "name": name,
        "file_bytes": file_bytes,
        "block_count": block_count,
        "head_count": 4,
        "head_count_kv": head_count_kv,
        "embedding_length": 128,
        "key_length": 128,
        "value_length": 128,
        "context_length": 512,
    }), encoding="utf-8")


def probe_capacity_only(profile: Path, label: str) -> dict:
    """--model-profile + --capacity-only 一次性进程：早退、不 listen，profile 参与预算。"""
    env = dict(os.environ)
    env["LLMOPT_ALLOW_STUB"] = "1"
    port = free_port()
    cmd = [str(SERVER), "--model-profile", str(profile), "--capacity-only",
           "--host", "127.0.0.1", "--port", str(port), "--backend", "stub",
           "--mode", "off", "--ctx-tokens", "512", "--log-level", "warn"]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=30, env=env)
    output = proc.stdout + proc.stderr
    # 早退进程退出后，所选端口上不应有任何监听套接字（未走到 listen）。
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        listened = sock.connect_ex(("127.0.0.1", port)) == 0
    return {"label": label, "exit_code": proc.returncode, "output": output,
            "port_listened": listened}



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

        # R3-01：--model-profile + --capacity-only 主启动路径进程级回归。
        small_profile = Path(tmp) / "capacity_small.json"
        write_profile(small_profile, 100 * 1024 * 1024, 2, 2, "stub-capacity")
        small = probe_capacity_only(small_profile, "capacity_only_eligible")
        check(small["exit_code"] == 0,
              f"小档位 capacity-only 应退出 0，实际 {small['exit_code']}\n{small['output']}")
        check("capacity-profile:" in small["output"], "缺少 capacity-profile 行")
        check("stub-capacity" in small["output"], "profile 名称未进入启动摘要")
        check("capacity-only: allow_start=1" in small["output"], "缺少 allow_start=1 行")
        check("decision=ELIGIBLE" in small["output"], "小档位决策应为 ELIGIBLE")
        check("estimate: model=" in small["output"], "缺少预算估算行")
        check(not small["port_listened"], "capacity-only 不应监听端口")

        big_profile = Path(tmp) / "capacity_big.json"
        write_profile(big_profile, 100 * 1024 ** 3, 128, 64, "capacity-oversized")
        big = probe_capacity_only(big_profile, "capacity_only_blocked")
        check(big["exit_code"] in (3, 4),
              f"超预算 capacity-only 应退出 3/4，实际 {big['exit_code']}\n{big['output']}")
        check("capacity-only: allow_start=0" in big["output"], "缺少 allow_start=0 行")
        check(not big["port_listened"], "被拒档位同样不应监听端口")
        small.pop("output", None)
        big.pop("output", None)
        gate_profiles.append(small)
        gate_profiles.append(big)

    if EVIDENCE_DIR is not None:
        EVIDENCE_DIR.mkdir(parents=True, exist_ok=True)
        (EVIDENCE_DIR / "gate_profiles.json").write_text(
            json.dumps(gate_profiles, ensure_ascii=False, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
