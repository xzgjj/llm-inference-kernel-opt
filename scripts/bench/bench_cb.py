#!/usr/bin/env python3
"""bench_cb.py — 连续批吞吐/延迟曲线 + 阶段 2 指标采样。

按 P=1/2/4... 发并发 SSE 请求，记录 TTFT/ITL 分位、吞吐、KV 稳态利用率、
调度器 CPU 占比、调度单步 p99、批大小峰值与结束时的 held/reconcile。

用法：
    python scripts/bench/bench_cb.py --port 18080 --concurrency 1,2,4 \
        --requests 8 --max-tokens 32 --output evidence/cb_curve.csv
    # 2-G2 稳态利用率：长 prompt 填满 KV，8 路并发
    python scripts/bench/bench_cb.py --port 18080 --concurrency 8 \
        --requests 8 --prompt-repeat 30 --max-tokens 32 --assert-gates
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


def percentile(values: list[float], pct: float) -> float:
    """线性插值分位；空列表返回 0。"""
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return float(ordered[0])
    rank = (len(ordered) - 1) * pct / 100.0
    low = int(rank)
    high = min(low + 1, len(ordered) - 1)
    weight = rank - low
    return float(ordered[low]) * (1.0 - weight) + float(ordered[high]) * weight


def metrics_snapshot(port: int) -> dict[str, float]:
    """抓一次 /metrics，返回 name（含 label）→ 数值。"""
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        conn.request("GET", "/metrics")
        body = conn.getresponse().read().decode("utf-8", "replace")
    finally:
        conn.close()
    out: dict[str, float] = {}
    for line in body.splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.rsplit(" ", 1)
        if len(parts) != 2:
            continue
        try:
            out[parts[0]] = float(parts[1])
        except ValueError:
            continue
    return out


def stream_once(port: int, prompt: str, max_tokens: int, result: dict, error: list[Exception]) -> None:
    """一次流式请求：记录 TTFT 与逐 token 间隔，结束后写 result。"""
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=300)
    started = time.perf_counter()
    times: list[float] = []
    try:
        body = json.dumps({"prompt": prompt, "stream": True, "max_tokens": max_tokens,
                           "seed": 7, "temperature": 0.0, "top_k": 1})
        conn.request("POST", "/completion", body, {"Content-Type": "application/json"})
        response = conn.getresponse()
        if response.status != 200:
            raise RuntimeError(f"HTTP {response.status}")
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
            json.loads(payload)
            times.append(time.perf_counter())
        intervals = [times[index] - times[index - 1] for index in range(1, len(times))]
        result["count"] = len(times)
        result["ttft_ms"] = (times[0] - started) * 1000.0 if times else 0.0
        result["latency_ms"] = (time.perf_counter() - started) * 1000.0
        result["itl_ms"] = [value * 1000.0 for value in intervals]
    except Exception as exc:  # noqa: BLE001 — 线程内异常汇总到主线程
        error.append(exc)
    finally:
        conn.close()


class Sampler:
    """后台轮询 /metrics，保留整轮请求期间的快照。"""

    def __init__(self, port: int, interval: float) -> None:
        self.port = port
        self.interval = interval
        self.samples: list[dict[str, float]] = []
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._loop)

    def _loop(self) -> None:
        while not self._stop.is_set():
            try:
                self.samples.append(metrics_snapshot(self.port))
            except Exception:  # noqa: BLE001 — 采样失败不拖垮压测
                pass
            time.sleep(self.interval)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join()


def snapshot_value(snapshot: dict[str, float], name: str) -> float:
    return snapshot.get(name, 0.0)


def usage(snapshot: dict[str, float]) -> tuple[float, float, float, float]:
    """返回 held(活跃)、cached、free、watermark。"""
    used = snapshot_value(snapshot, "llmopt_kv_used_blocks")
    cached = snapshot_value(snapshot, "llmopt_kv_cached_blocks")
    free = snapshot_value(snapshot, "llmopt_kv_free_blocks")
    watermark = snapshot_value(snapshot, "llmopt_kv_watermark_blocks")
    return used - cached, cached, free, watermark


def run_level(port: int, requests: int, concurrency: int, max_tokens: int,
              prompt_repeat: int, prompts: list[str] | None = None) -> dict:
    """按并发度跑完 requests 条，统计吞吐、延迟与阶段 2 指标。"""
    cursor = 0
    lock = threading.Lock()
    first: list[float] = []
    tokens: list[int] = []
    latencies: list[float] = []
    intervals: list[float] = []
    errors: list[Exception] = []
    start_metrics = metrics_snapshot(port)
    sampler = Sampler(port, 0.15)
    sampler.start()

    def worker() -> None:
        nonlocal cursor
        while True:
            with lock:
                if cursor >= requests:
                    return
                current = cursor
                cursor += 1
            prompt = (prompts[current % len(prompts)] if prompts else
                      f"请求{current}：连续批处理通过合并多个请求的前向计算来提升推理吞吐。"
                      * prompt_repeat)
            result: dict = {"count": 0, "ttft_ms": 0.0, "latency_ms": 0.0, "itl_ms": []}
            stream_once(port, prompt, max_tokens, result, errors)
            first.append(result["ttft_ms"])
            tokens.append(result["count"])
            latencies.append(result["latency_ms"])
            intervals.extend(result["itl_ms"])

    started = time.perf_counter()
    threads = [threading.Thread(target=worker) for _ in range(concurrency)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    elapsed = time.perf_counter() - started
    sampler.stop()
    if errors:
        raise RuntimeError(f"请求失败: {errors}")

    # 等块表收尾后再抓终态，避免最后一帧已发、KV 还没 release 的竞态。
    end_metrics: dict[str, float] = {}
    for _ in range(20):
        end_metrics = metrics_snapshot(port)
        if usage(end_metrics)[0] == 0:
            break
        time.sleep(0.05)
    sched_ns = (snapshot_value(end_metrics, "llmopt_scheduler_ns_total") -
                snapshot_value(start_metrics, "llmopt_scheduler_ns_total"))
    sched_p99_ns = snapshot_value(end_metrics, "llmopt_schedule_step_p99_ns")
    active_samples = [sample for sample in sampler.samples
                      if snapshot_value(sample, "llmopt_running_sequences") > 0]
    util_samples: list[float] = []
    for sample in active_samples:
        used = snapshot_value(sample, "llmopt_kv_used_blocks")
        free = snapshot_value(sample, "llmopt_kv_free_blocks")
        watermark = snapshot_value(sample, "llmopt_kv_watermark_blocks")
        # 物理占用口径：cached donor 同样占块，只是可被淘汰；2-G2 看块池是否真正吃满。
        usable = used + free - watermark
        if usable > 0:
            util_samples.append(max(0.0, used - watermark) / usable * 100.0)
    held_end, cached_end, free_end, watermark_end = usage(end_metrics)

    ttft_p95 = percentile(first, 95)
    itl_p50 = percentile(intervals, 50)
    itl_p99 = percentile(intervals, 99)
    return {
        "concurrency": concurrency,
        "requests": requests,
        "tokens": sum(tokens),
        "seconds": round(elapsed, 3),
        "tokens_per_s": round(sum(tokens) / elapsed, 2),
        "ttft_p50_ms": round(percentile(first, 50), 2),
        "ttft_p95_ms": round(ttft_p95, 2),
        "ttft_p99_ms": round(percentile(first, 99), 2),
        "itl_p50_ms": round(itl_p50, 2),
        "itl_p99_ms": round(itl_p99, 2),
        "itl_p99_over_p50": round(itl_p99 / itl_p50, 2) if itl_p50 > 0 else 0.0,
        "latency_p50_ms": round(percentile(latencies, 50), 2),
        "kv_util_median_pct": round(percentile(util_samples, 50), 2),
        "running_peak": int(max((snapshot_value(sample, "llmopt_running_sequences")
                                 for sample in sampler.samples), default=0)),
        "batch_size_peak": int(snapshot_value(end_metrics, "llmopt_batch_size_peak")),
        "scheduler_cpu_pct": round(sched_ns / (elapsed * 1e9) * 100.0, 3),
        "schedule_step_p99_us": round(sched_p99_ns / 1000.0, 3),
        "held_end": int(held_end),
        "cached_end": int(cached_end),
        "free_end": int(free_end),
        "watermark_end": int(watermark_end),
        "reconcile_diffs": int(snapshot_value(end_metrics,
                                              "llmopt_kv_reconcile_diffs_total")),
    }


def baseline_row(path: str, concurrency: int, round_index: int, tier: str) -> dict[str, str] | None:
    """从 phase0 baseline.csv 取同并发/同档对照行。"""
    with Path(path).open(encoding="utf-8", newline="") as handle:
        for row in csv.DictReader(handle):
            if (row.get("concurrency_p") == str(concurrency) and
                    row.get("round") == str(round_index) and row.get("tier") == tier):
                return row
    return None


def attach_baseline(rows: list[dict], args: argparse.Namespace) -> None:
    if not args.baseline_csv:
        return
    for row in rows:
        base = baseline_row(args.baseline_csv, row["concurrency"], args.baseline_round,
                            args.baseline_tier)
        if base:
            base_ttft = float(base["ttft_p95_ms"])
            row["b0_ttft_p95_ms"] = round(base_ttft, 2)
            row["ttft_p95_ratio"] = round(row["ttft_p95_ms"] / base_ttft, 2) if base_ttft else 0.0


def check_gates(rows: list[dict], gates: set[str]) -> bool:
    """按显式 gate 集合判定，避免短 prompt 曲线被 75% 利用率误判。"""
    ok = True
    for row in rows:
        if "2g2" in gates and row["kv_util_median_pct"] < 75.0:
            print(f"FAIL 2-G2 util={row['kv_util_median_pct']}% < 75%")
            ok = False
        if "2g4" in gates and row["scheduler_cpu_pct"] > 5.0:
            print(f"FAIL 2-G4 sched_cpu={row['scheduler_cpu_pct']}% > 5%")
            ok = False
        if "2g5" in gates and row["itl_p99_over_p50"] > 3.0:
            print(f"FAIL 2-G5 itl_p99/p50={row['itl_p99_over_p50']} > 3")
            ok = False
        if ("2g5" in gates and row.get("ttft_p95_ratio", 0.0) != 0.0 and
                row["ttft_p95_ratio"] > 1.2):
            print(f"FAIL 2-G5 ttft_p95_ratio={row['ttft_p95_ratio']} > 1.2")
            ok = False
    return ok


def load_prompts(path: str, tier: str) -> list[str]:
    """读 phase0 prompts_seed.jsonl，保证 2-G5 与 B0 用同一批 prompt。"""
    prompts: list[str] = []
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            record = json.loads(line)
            if tier and record.get("tier") != tier:
                continue
            prompts.append(record["text"])
    if not prompts:
        raise ValueError(f"no prompt in {path} for tier={tier!r}")
    return prompts


def warmup(port: int, prompts: list[str], count: int, max_tokens: int) -> None:
    """顺序预热，排除冷启动对 TTFT p95 的影响（phase0 用 5 条）。"""
    errors: list[Exception] = []
    for index in range(count):
        prompt = prompts[index % len(prompts)]
        stream_once(port, prompt, min(max_tokens, 8), {"count": 0, "itl_ms": []}, errors)
        if errors:
            raise RuntimeError(f"预热失败: {errors}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--concurrency", default="1,2,4")
    parser.add_argument("--requests", type=int, default=8)
    parser.add_argument("--max-tokens", type=int, default=32)
    parser.add_argument("--prompt-repeat", type=int, default=1,
                        help="基础句重复次数；2-G2 用 30 左右把 KV 填到 75 以上")
    parser.add_argument("--prompts-file", default="",
                        help="phase0 prompts_seed.jsonl；用于与 B0 同 prompt 对比")
    parser.add_argument("--tier", default="short", help="prompts-file 的 tier 过滤")
    parser.add_argument("--warmup", type=int, default=0, help="计时前顺序预热请求数")
    parser.add_argument("--output", default="")
    parser.add_argument("--baseline-csv", default="",
                        help="phase0 baseline.csv，用于 2-G5 的 TTFT p95 对照")
    parser.add_argument("--baseline-round", type=int, default=2)
    parser.add_argument("--baseline-tier", default="short")
    parser.add_argument("--assert-gates", action="store_true")
    parser.add_argument("--gates", default="",
                        help="逗号分隔的 gate 集：2g2,2g4,2g5；空表示不判定")
    args = parser.parse_args()

    levels = [int(value) for value in args.concurrency.split(",") if value]
    prompts = load_prompts(args.prompts_file, args.tier) if args.prompts_file else None
    if prompts and args.warmup:
        warmup(args.port, prompts, args.warmup, args.max_tokens)
    rows = [run_level(args.port, args.requests, level, args.max_tokens, args.prompt_repeat,
                      prompts) for level in levels]
    attach_baseline(rows, args)
    fields = list(rows[0])
    for row in rows:
        print(json.dumps(row, ensure_ascii=False))
    if args.output:
        path = Path(args.output)
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)
    if args.assert_gates and not check_gates(
            rows, {value.strip() for value in args.gates.split(",") if value.strip()}):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
