#!/usr/bin/env python3
"""bench_cb.py — 连续批吞吐曲线：固定服务端，按 P=1/2/4 发并发 SSE 请求。

用法：
    python scripts/bench/bench_cb.py --port 18080 --concurrency 1,2,4 \
        --requests 8 --max-tokens 32 --output evidence/cb_curve.csv

输出列：concurrency, requests, tokens, seconds, tokens_per_s, ttft_p50_ms, latency_p50_ms。
只依赖标准库；stub 与 llama 后端都适用，stub 数字仅用于流程校验，不作性能结论。
"""
from __future__ import annotations

import argparse
import csv
import http.client
import json
import statistics
import threading
import time
from pathlib import Path


def stream_once(port: int, prompt: str, max_tokens: int, first_token: list[float],
                error: list[Exception]) -> int:
    """发一次流式请求，返回生成 token 数；首帧时间写入 first_token。"""
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=300)
    started = time.perf_counter()
    try:
        body = json.dumps({"prompt": prompt, "stream": True, "max_tokens": max_tokens,
                           "seed": 7})
        conn.request("POST", "/completion", body, {"Content-Type": "application/json"})
        response = conn.getresponse()
        if response.status != 200:
            raise RuntimeError(f"HTTP {response.status}")
        count = 0
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
            frame = json.loads(payload)
            if count == 0:
                first_token.append((time.perf_counter() - started) * 1000.0)
            count += 1
        return count
    except Exception as exc:  # noqa: BLE001 — 线程内异常汇总到主线程
        error.append(exc)
        return 0
    finally:
        conn.close()


def run(port: int, requests: int, concurrency: int, max_tokens: int) -> dict:
    """按并发度跑完 requests 条，统计吞吐与分位延迟。"""
    cursor = 0
    lock = threading.Lock()
    first: list[float] = []
    errors: list[Exception] = []
    tokens: list[int] = []
    latencies: list[float] = []

    def worker(index: int) -> None:
        nonlocal cursor
        while True:
            with lock:
                if cursor >= requests:
                    return
                current = cursor
                cursor += 1
            prompt = f"请用一句话回答第 {current} 个问题：连续批处理如何提升推理吞吐？"
            started = time.perf_counter()
            count = stream_once(port, prompt, max_tokens, first, errors)
            latencies.append((time.perf_counter() - started) * 1000.0)
            tokens.append(count)

    started = time.perf_counter()
    threads = [threading.Thread(target=worker, args=(index,)) for index in range(concurrency)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    elapsed = time.perf_counter() - started
    if errors:
        raise RuntimeError(f"请求失败: {errors}")
    return {
        "concurrency": concurrency,
        "requests": requests,
        "tokens": sum(tokens),
        "seconds": round(elapsed, 3),
        "tokens_per_s": round(sum(tokens) / elapsed, 2),
        "ttft_p50_ms": round(statistics.median(first), 2) if first else 0.0,
        "latency_p50_ms": round(statistics.median(latencies), 2) if latencies else 0.0,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--concurrency", default="1,2,4")
    parser.add_argument("--requests", type=int, default=8)
    parser.add_argument("--max-tokens", type=int, default=32)
    parser.add_argument("--output", default="")
    args = parser.parse_args()

    levels = [int(value) for value in args.concurrency.split(",") if value]
    rows = [run(args.port, args.requests, level, args.max_tokens) for level in levels]
    for row in rows:
        print(json.dumps(row, ensure_ascii=False))
    if args.output:
        path = Path(args.output)
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
