# test_server_chunked.py — 进程级 3-G1：chunk 64 与整段 prefill 的 SSE 逐 token 一致。
# 复用并发用例的 GGUF/端口/探活助手；stub 后端保证无 GPU 依赖。
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_server_concurrency import (  # noqa: E402 — 同目录助手需先加 sys.path
    free_port,
    get,
    make_gguf,
    metric,
    post,
    wait_ready,
)

SERVER = Path(sys.argv[1])


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def stream_tokens(port: int, prompt: str, max_tokens: int = 8) -> list[int]:
    """发 SSE 并返回 token_id 序列；[DONE] 与 finish 帧不计入 token。"""
    status, body = post(port, "/completion",
                        {"prompt": prompt, "stream": True,
                         "max_tokens": max_tokens, "seed": 7})
    check(status == 200, f"流式请求应 200，实际 {status}")
    lines = [line for line in body.decode().splitlines() if line.startswith("data: ")]
    check(lines and lines[-1] == "data: [DONE]", "SSE 缺少 [DONE]")
    tokens: list[int] = []
    for line in lines[:-1]:
        frame = json.loads(line[len("data: "):])
        choice = frame["choices"][0]
        if "token_id" in choice:
            tokens.append(choice["token_id"])
    return tokens


def run_case(server: Path, model: Path, env: dict[str, str], chunk: int, budget: int,
             prompt: str) -> tuple[list[int], bytes]:
    """起一次服务、跑一条请求，返回 token 序列与 /metrics 文本。"""
    port = free_port()
    cmd = [str(server), "--model", str(model), "--host", "127.0.0.1", "--port", str(port),
           "--backend", "stub", "--mode", "off", "--ctx-tokens", "512",
           "--max-concurrency", "2", "--max-num-batched-tokens", str(budget),
           "--chunk-size", str(chunk), "--block-size", "8", "--queue-capacity", "4",
           "--log-level", "warn"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    try:
        wait_ready(port, proc)
        tokens = stream_tokens(port, prompt)
        status, body = get(port, "/metrics")
        check(status == 200, "metrics 应 200")
        return tokens, body
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        model = Path(tmp) / "fake.gguf"
        make_gguf(model)
        env = dict(os.environ)
        env["LLMOPT_ALLOW_STUB"] = "1"
        prompt = "chunk-prefill-" + "x" * 190

        chunked, chunked_metrics = run_case(SERVER, model, env, 64, 256, prompt)
        whole, whole_metrics = run_case(SERVER, model, env, 256, 256, prompt)
        check(len(chunked) == 8, f"chunked 应产出 8 token，实际 {len(chunked)}")
        check(chunked == whole, "chunk 64 与 chunk 256 输出应逐 token 一致")
        check(metric(chunked_metrics, "llmopt_chunk_prefill_steps_total") >= 2,
              "chunk 64 应记录至少 2 个非末块")
        check(metric(chunked_metrics, "llmopt_chunk_prefill_tokens_total") >= 128,
              "非末块 prefill token 数应与切块一致")
        check(metric(whole_metrics, "llmopt_chunk_prefill_steps_total") == 0,
              "prompt 小于 256 时不应产生中间 chunk")
        check(metric(chunked_metrics, "llmopt_kv_reconcile_diffs_total") == 0,
              "chunked prefill 的 KV 位置对账应零差异")

        # 阶段 2 的启动限制已放开：prompt 长于单步预算仍能推进，不会队头挂死。
        long_prompt = "budget-" + "y" * 300
        long_tokens, long_metrics = run_case(SERVER, model, env, 64, 128, long_prompt)
        check(len(long_tokens) == 8, f"预算 128 下长 prompt 应完成 8 token，实际 {len(long_tokens)}")
        check(metric(long_metrics, "llmopt_chunk_deferred_total") >= 0,
              "chunk 指标应存在")
        print("OK integration_server_chunked")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
