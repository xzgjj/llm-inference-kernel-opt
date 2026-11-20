#!/usr/bin/env python3
"""slow_client.py — 慢读客户端隔离探测（4-G0-2）。

一个 SSE 客户端每帧睡 1s，同时另一个快客户端完整读流；记录快客户端 ITL p99
与 sink 溢出计数，证明慢消费者不阻塞生产者、不拖垮其他请求。
"""
from __future__ import annotations

import argparse
import csv
import http.client
import json
import sys
import threading
import time
import urllib.parse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.bench import aggregate  # noqa: E402


def _conn(base: str) -> tuple[http.client.HTTPConnection, str, int]:
    parsed = urllib.parse.urlparse(base)
    return http.client.HTTPConnection(parsed.hostname, parsed.port, timeout=60), parsed.hostname, parsed.port


def slow_sse(base: str, tokens: int, delay: float, stop_after: int) -> dict:
    conn, _, _ = _conn(base)
    frames = 0
    started = time.monotonic()
    try:
        body = json.dumps({"prompt": "slow client", "max_tokens": tokens, "stream": True})
        conn.request("POST", "/v1/completions", body=body,
                     headers={"Content-Type": "application/json"})
        response = conn.getresponse()
        while True:
            line = response.readline()
            if not line:
                break
            text = line.decode("utf-8", "replace").strip()
            if text.startswith("data:") and text != "data: [DONE]":
                frames += 1
                if frames >= stop_after:
                    break
                time.sleep(delay)
    finally:
        conn.close()
    return {"frames": frames, "wall_s": time.monotonic() - started}


def fast_read(base: str, tokens: int) -> dict:
    conn, _, _ = _conn(base)
    gaps: list[float] = []
    last = None
    text = ""
    try:
        body = json.dumps({"prompt": "fast client", "max_tokens": tokens, "stream": True})
        conn.request("POST", "/v1/completions", body=body,
                     headers={"Content-Type": "application/json"})
        response = conn.getresponse()
        while True:
            line = response.readline()
            if not line:
                break
            payload = line.decode("utf-8", "replace").strip()
            if not payload.startswith("data:") or payload == "data: [DONE]":
                if payload == "data: [DONE]":
                    break
                continue
            now = time.monotonic()
            if last is not None:
                gaps.append((now - last) * 1000.0)
            last = now
            text += payload
    finally:
        conn.close()
    return {"frames": len(gaps) + 1 if last else 0, "gaps_ms": gaps, "text": text}


def metric(base: str, name: str) -> float | None:
    conn, _, _ = _conn(base)
    try:
        conn.request("GET", "/metrics")
        body = conn.getresponse().read().decode("utf-8", "replace")
    finally:
        conn.close()
    value = None
    for line in body.splitlines():
        if line.startswith(name + " ") or line.startswith(name + "{"):
            value = float(line.rsplit(" ", 1)[1])
    return value


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="慢读隔离探测")
    parser.add_argument("--base", default="http://127.0.0.1:18080")
    parser.add_argument("--out", required=True)
    parser.add_argument("--tokens", type=int, default=128)
    parser.add_argument("--frames", type=int, default=3)
    parser.add_argument("--delay", type=float, default=1.0)
    args = parser.parse_args(argv)

    overflow_before = metric(args.base, "llmopt_sink_overflow_total")
    slow_result: dict = {}
    slow = threading.Thread(target=lambda: slow_result.update(
        slow_sse(args.base, args.tokens, args.delay, args.frames)))
    slow.start()
    time.sleep(0.2)
    fast = fast_read(args.base, 64)
    slow.join()
    overflow_after = metric(args.base, "llmopt_sink_overflow_total")

    p99 = aggregate.percentile(fast["gaps_ms"], 0.99) if fast["gaps_ms"] else None
    row = {
        "case": "slow_client_isolation",
        "fast_itl_p99_ms": "" if p99 is None else round(p99, 3),
        "fast_frames": fast["frames"],
        "slow_frames": slow_result.get("frames", 0),
        "overflow_before": overflow_before,
        "overflow_after": overflow_after,
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(row))
        writer.writeheader()
        writer.writerow(row)
    ok = fast["frames"] > 0 and slow_result.get("frames", 0) >= 1 and overflow_after is not None
    print(f"[slow] fast_frames={fast['frames']} p99={row['fast_itl_p99_ms']} "
          f"slow_frames={row['slow_frames']} overflow={overflow_before}->{overflow_after}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
