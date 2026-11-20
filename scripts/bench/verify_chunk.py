#!/usr/bin/env python3
"""verify_chunk.py — 真实后端 3-G1 验收：不同 chunk 宽度的逐 token 输出必须一致。

同一进程参数只改 chunk-size，跑同一 prompt、同 seed、贪心采样，比较 token 序列；
同时读取 chunk 指标，确认窄档确实产生了中间块。仅标准库，不依赖 GPU 工具。
"""
from __future__ import annotations

import argparse
import http.client
import json
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


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


def stream(port: int, prompt: str, max_tokens: int) -> list[int]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=120)
    try:
        body = json.dumps({"prompt": prompt, "stream": True, "max_tokens": max_tokens,
                           "seed": 7, "temperature": 0.0, "top_k": 1,
                           "top_p": 1.0, "min_p": 0.0, "repeat_penalty": 1.0})
        conn.request("POST", "/completion", body, {"Content-Type": "application/json"})
        response = conn.getresponse()
        if response.status != 200:
            raise RuntimeError(f"HTTP {response.status}")
        ids: list[int] = []
        while True:
            line = response.readline()
            if not line:
                break
            text = line.decode("utf-8", "replace").strip()
            if not text.startswith("data: "):
                continue
            payload = text[len("data: "):]
            if payload == "[DONE]":
                break
            ids.append(json.loads(payload)["choices"][0]["token_id"])
        return ids
    finally:
        conn.close()


def metric(body: bytes, name: str) -> int:
    for line in body.decode().splitlines():
        if line.startswith(name + " "):
            return int(line.rsplit(" ", 1)[1])
    return 0


def run_once(server: str, model: str, chunk: int, prompt: str, max_tokens: int,
             extra: list[str]) -> tuple[list[int], int]:
    port = free_port()
    log = tempfile.NamedTemporaryFile(prefix="llmopt-chunk-", suffix=".log", delete=False)
    log.close()
    cmd = [server, "--model", model, "--host", "127.0.0.1", "--port", str(port),
           "--backend", "llama", "--mode", "off", "--ctx-tokens", "2048",
           "--max-concurrency", "2", "--max-num-batched-tokens", "1024",
           "--chunk-size", str(chunk), "--block-size", "16", "--log-level", "warn", *extra]
    with open(log.name, "wb") as sink:
        proc = subprocess.Popen(cmd, stdout=sink, stderr=sink)
        try:
            deadline = time.time() + 90
            while time.time() < deadline:
                if proc.poll() is not None:
                    tail = Path(log.name).read_text(errors="replace").splitlines()[-20:]
                    raise RuntimeError(f"服务提前退出 rc={proc.returncode}\n" + "\n".join(tail))
                try:
                    if get(port, "/healthz")[0] == 200:
                        break
                except OSError:
                    pass
                time.sleep(0.2)
            else:
                raise RuntimeError("等待 /healthz 就绪超时")
            tokens = stream(port, prompt, max_tokens)
            status, body = get(port, "/metrics")
            if status != 200:
                raise RuntimeError("metrics 读取失败")
            return tokens, metric(body, "llmopt_chunk_prefill_steps_total")
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
            Path(log.name).unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", default="./build-wsl/llmopt_server")
    parser.add_argument("--model", default="/root/llm-src/models/qwen2.5-0.5b-q4.gguf")
    parser.add_argument("--chunk-a", type=int, default=32)
    parser.add_argument("--chunk-b", type=int, default=256)
    parser.add_argument("--tokens", type=int, default=8)
    parser.add_argument("--n-gpu-layers", type=int, default=0)
    args = parser.parse_args()
    extra = ["--threads", "8", "--n-gpu-layers", str(args.n_gpu_layers)]
    prompt = "The quick brown fox jumps over the lazy dog. " * 30

    first, chunks = run_once(args.server, args.model, args.chunk_a, prompt, args.tokens, extra)
    second, _ = run_once(args.server, args.model, args.chunk_b, prompt, args.tokens, extra)
    print(f"chunk={args.chunk_a} tokens={first} chunk_steps={chunks}")
    print(f"chunk={args.chunk_b} tokens={second}")
    if len(first) != args.tokens or len(second) != args.tokens:
        raise AssertionError(f"生成不完整: {len(first)}/{len(second)}")
    if first != second:
        raise AssertionError("不同 chunk 宽度的逐 token 输出不一致")
    if chunks <= 0:
        raise AssertionError(f"chunk={args.chunk_a} 未产生中间块")
    print("OK chunked-prefill token-equivalent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
