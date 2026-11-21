# nightly_real_backend.py — 真后端 nightly：起服务并验证 healthz/readyz、非流式、
# SSE、客户端断开取消、/metrics 五组行为；失败即非零退出，不因“环境跑不了”跳过。
from __future__ import annotations

import argparse
import hashlib
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path


def fail(message: str) -> int:
    print(f"FAIL: {message}", file=sys.stderr)
    return 1


def http_json(base: str, path: str, payload: dict | None = None, timeout: float = 30.0) -> tuple[int, dict]:
    body = json.dumps(payload).encode() if payload is not None else None
    headers = {"Content-Type": "application/json"} if body else {}
    request = urllib.request.Request(base + path, data=body, headers=headers, method="POST" if body else "GET")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read()
            return response.status, json.loads(raw) if raw else {}
    except urllib.error.HTTPError as exc:
        raw = exc.read()
        try:
            return exc.code, json.loads(raw) if raw else {}
        except json.JSONDecodeError:
            return exc.code, {}


def http_text(base: str, path: str, timeout: float = 30.0) -> tuple[int, str]:
    request = urllib.request.Request(base + path, method="GET")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status, response.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read().decode("utf-8", "replace")


def metric_value(text: str, series: str) -> float:
    for line in text.splitlines():
        if line.startswith(series):
            try:
                return float(line.split()[-1])
            except ValueError:
                return 0.0
    return 0.0


def wait_ready(base: str, timeout: float, process: subprocess.Popen) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            return False
        try:
            status, _ = http_text(base, "/readyz", timeout=2)
            if status == 200:
                return True
        except (urllib.error.URLError, ConnectionError, TimeoutError):
            pass
        time.sleep(0.5)
    return False


def sse_full(base: str) -> tuple[bool, str]:
    request = urllib.request.Request(
        base + "/completion",
        data=json.dumps({"prompt": "hello", "stream": True, "max_tokens": 4, "seed": 42}).encode(),
        headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            if response.status != 200:
                return False, f"SSE 应 200，实际 {response.status}"
            text = response.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as exc:
        return False, f"SSE 应 200，实际 {exc.code}"
    lines = [line for line in text.splitlines() if line.startswith("data: ")]
    if not lines or lines[-1] != "data: [DONE]":
        return False, "SSE 未以 [DONE] 结束"
    try:
        first = json.loads(lines[0][6:])
    except json.JSONDecodeError:
        return False, "SSE 首帧不是 JSON"
    if not first.get("choices"):
        return False, "SSE 首帧缺 choices"
    return True, ""


def raw_sse_cancel(base: str) -> bool:
    host_port = base.removeprefix("http://")
    host, _, port_text = host_port.partition(":")
    port = int(port_text)
    body = json.dumps({
        "prompt": "token " * 200,
        "stream": True,
        "max_tokens": 64,
        "seed": 7,
    }).encode()
    request = (
        f"POST /completion HTTP/1.1\r\nHost: {host}\r\nContent-Type: application/json\r\n"
        f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n"
    ).encode() + body
    with socket.create_connection((host, port), timeout=15) as sock:
        sock.sendall(request)
        sock.settimeout(15)
        data = b""
        while b"data: " not in data:
            chunk = sock.recv(4096)
            if not chunk:
                break
            data += chunk
    return b"data: " in data


def wait_cancel_metric(base: str, before: float, timeout: float) -> tuple[bool, str]:
    expected = before + 1.0
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        status, text = http_text(base, "/metrics", timeout=5)
        if status == 200:
            observed = metric_value(text, 'llmopt_requests_total{status="cancelled"}')
            if observed == expected:
                return True, ""
            if observed > expected:
                return False, f"cancelled 指标多计：期望 {expected}，实际 {observed}"
        time.sleep(0.25)
    return False, "cancelled 指标未按预期 +1"


def wait_kv_zero(base: str, timeout: float) -> tuple[bool, float]:
    deadline = time.monotonic() + timeout
    last = -1.0
    while time.monotonic() < deadline:
        status, text = http_text(base, "/metrics", timeout=5)
        if status == 200:
            last = metric_value(text, "llmopt_kv_used_blocks")
            if last == 0.0:
                return True, last
        time.sleep(0.1)
    return False, last


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--port", type=int, default=18080)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--ngl", type=int, default=0)
    parser.add_argument("--ctx", type=int, default=512)
    parser.add_argument("--expect-sha256", default="", help="固定模型哈希；提供时启动前校验，不匹配直接失败")
    parser.add_argument("--evidence", default="")
    args = parser.parse_args()

    model = Path(args.model)
    if not model.is_file():
        return fail(f"模型不存在: {model}")
    model_sha256 = file_sha256(model)
    if args.expect_sha256 and model_sha256 != args.expect_sha256.lower():
        return fail(f"模型 sha256 不匹配：期望 {args.expect_sha256}，实际 {model_sha256}")

    base = f"http://127.0.0.1:{args.port}"
    log_path = Path(tempfile.gettempdir()) / f"llmopt-nightly-real-{args.port}.log"
    with log_path.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(
            [args.server, "--model", str(model), "--host", "127.0.0.1", "--port", str(args.port),
             "--backend", "llama", "--mode", "off", "--ctx-tokens", str(args.ctx),
             "--max-concurrency", "1", "--queue-capacity", "4", "--threads", str(args.threads),
             "--n-gpu-layers", str(args.ngl), "--drain-deadline-seconds", "10", "--log-level", "info"],
            stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
        )
        try:
            if not wait_ready(base, args.timeout, process):
                return fail(f"服务未就绪，日志:\n{log_path.read_text(encoding='utf-8', errors='replace')[-3000:]}")

            checks: dict[str, bool] = {}
            status, health = http_json(base, "/healthz")
            checks["healthz"] = status == 200 and health.get("status") == "ready"
            status, ready = http_json(base, "/readyz")
            checks["readyz"] = status == 200 and ready.get("status") == "ready"

            status, completion = http_json(base, "/completion", {"prompt": "hello", "max_tokens": 4, "seed": 42})
            usage = completion.get("usage", {}) if isinstance(completion.get("usage"), dict) else {}
            checks["non_stream"] = (
                status == 200
                and bool(completion.get("choices"))
                and usage.get("total_tokens", 0) == usage.get("prompt_tokens", 0) + usage.get("completion_tokens", 0)
            )

            status, metrics_before = http_text(base, "/metrics")
            if status != 200:
                return fail(f"/metrics 应 200，实际 {status}")
            cancelled_before = metric_value(metrics_before, 'llmopt_requests_total{status="cancelled"}')

            ok, sse_error = sse_full(base)
            checks["sse"] = ok
            if not ok:
                print(sse_error)

            checks["sse_first_frame"] = raw_sse_cancel(base)
            checks["cancel"], cancel_error = wait_cancel_metric(base, cancelled_before, 15.0)
            if not checks["cancel"]:
                print(cancel_error)

            status, metrics_after = http_text(base, "/metrics")
            checks["metrics"] = status == 200 and "llmopt_requests_total" in metrics_after \
                and "llmopt_kv_used_blocks" in metrics_after

            # 取消后服务仍可用：再跑一次非流式，确认没有卡死或 KV 泄漏。
            status, completion_after = http_json(base, "/completion", {"prompt": "hello", "max_tokens": 2, "seed": 1})
            checks["post_cancel_healthy"] = status == 200 and bool(completion_after.get("choices"))

            checks["kv_released"], kv_used_after = wait_kv_zero(base, 5.0)

            evidence = {
                "checks": checks,
                "metrics": {
                    "cancelled_before": cancelled_before,
                    "cancelled_after": metric_value(metrics_after, 'llmopt_requests_total{status="cancelled"}'),
                    "kv_used_blocks": kv_used_after,
                },
                "model": str(model),
                "model_sha256": model_sha256,
                "expect_sha256": args.expect_sha256,
                "port": args.port,
            }
            if args.evidence:
                Path(args.evidence).write_text(json.dumps(evidence, ensure_ascii=False, indent=2), encoding="utf-8")
            failed = [name for name, ok in checks.items() if not ok]
            if failed:
                return fail("失败项: " + ", ".join(failed))
            print("PASS: real backend nightly 5 组（health+ready/non-stream/SSE/cancel/metrics）")
            return 0
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
