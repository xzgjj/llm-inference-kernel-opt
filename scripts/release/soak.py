# soak.py — 阶段 12 S2：30 min 持续压测，只盯三件事（RSS 增长 / kv_used / ITL p99 漂移）。
#
# 产物（--out-dir）：
#   soak_report.json   汇总判据
#   rss.csv            ts_unix,rss_mib,rss_peak_mib
#   kv_used.csv        ts_unix,used_blocks,cached_blocks,free_blocks,reconcile_diffs
#   itl_samples.jsonl  ts_unix,request_id,itl_ms
#   server.log         服务端日志
#
# 判据（SPEC-DEBUG1 §5.3 + 用户裁决）：
#   RSS：末 10 min 均值 − 首 10 min 均值 ≤ 30 MiB
#   kv_used：稳态期不得"无新请求仍持续上升"；drain 后 used=0、cached=0、reconcile_diffs=0
#   ITL：末 10 min p99 ≤ 1.3 × 首 10 min p99
from __future__ import annotations

import argparse
import csv
import http.client
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests" / "integration"))
import server_kit as kit  # noqa: E402

FIRST_WINDOW_S = 600
LAST_WINDOW_S = 600
RSS_LIMIT_MIB = 30.0
P99_RATIO_LIMIT = 1.3


def one_request(port: int, index: int, max_tokens: int) -> tuple[int, list[float]]:
    """跑一条流式请求，返回 (状态码, 逐 token 间隔 ms 列表)。"""
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=120)
    stamps: list[float] = []
    try:
        payload = json.dumps({"prompt": f"soak-{index}", "stream": True,
                              "max_tokens": max_tokens, "seed": index % 97})
        conn.request("POST", "/v1/completions", payload, {"Content-Type": "application/json"})
        response = conn.getresponse()
        if response.status != 200:
            response.read()
            return response.status, []
        while True:
            line = response.readline()
            if not line:
                break
            if line.startswith(b"data: [DONE]"):
                break
            if line.startswith(b"data: "):
                stamps.append(time.time())
        return 200, [(b - a) * 1000.0 for a, b in zip(stamps, stamps[1:])]
    finally:
        conn.close()


def p99(values: list[float]) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    pos = min(len(ordered) - 1, int(round(0.99 * (len(ordered) - 1))))
    return ordered[pos]


def mean(values: list[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def write_csv(path: Path, header: list[str], rows: list[list[object]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(header)
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("server", type=Path)
    parser.add_argument("--seconds", type=int, default=1800)
    parser.add_argument("--interval", type=float, default=10.0)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--ctx-tokens", type=int, default=1024)
    parser.add_argument("--max-tokens", type=int, default=32)
    parser.add_argument("--out-dir", type=Path, default=None)
    parser.add_argument("--report", type=Path, default=None)
    parser.add_argument("--log-level", default="warn")
    args = parser.parse_args()

    out_dir = args.out_dir or Path(tempfile.mkdtemp(prefix="soak-out-"))
    out_dir.mkdir(parents=True, exist_ok=True)

    workdir = Path(tempfile.mkdtemp(prefix="soak-"))
    model = workdir / "soak.gguf"
    kit.make_gguf(model)
    port = kit.free_port()
    env = dict(os.environ)
    env["LLMOPT_ALLOW_STUB"] = "1"
    log_path = out_dir / "server.log"
    log_handle = log_path.open("w", encoding="utf-8")
    proc = subprocess.Popen(
        [str(args.server), "--model", str(model), "--host", "127.0.0.1", "--port", str(port),
         "--backend", "stub", "--mode", "off", "--ctx-tokens", str(args.ctx_tokens),
         "--max-num-batched-tokens", "1024", "--max-concurrency", str(args.workers),
         "--log-level", args.log_level],
        stdout=log_handle, stderr=subprocess.STDOUT, env=env, cwd=str(workdir))

    stop = threading.Event()
    counters = {"requests": 0, "errors": 0}
    lock = threading.Lock()
    rss_rows: list[list[object]] = []
    kv_rows: list[list[object]] = []
    itl_rows: list[list[object]] = []
    samples = {"rss": [], "used": [], "cached": [], "reconcile": []}

    def workload(worker: int) -> None:
        index = worker
        while not stop.is_set():
            try:
                status, itls = one_request(port, index, args.max_tokens)
            except OSError:
                status, itls = -1, []
            now = time.time()
            with lock:
                counters["requests"] += 1
                if status != 200:
                    counters["errors"] += 1
                # ITL 只做抽样落盘（1/50 请求），30 min 全量会在 GB 级；
                # p99 只用抽样分布，样本量仍 ≥ 数万条。
                if counters["requests"] % 50 == 0:
                    for value in itls:
                        itl_rows.append([round(now, 3), index, round(value, 3)])
                        samples_itl.append((now, value))
            index += args.workers
            time.sleep(0.02)

    samples_itl: list[tuple[float, float]] = []
    server_exit_code: int | None = None
    dead_at: float | None = None
    started = time.time()
    try:
        kit.wait_ready(port, proc, timeout=90)
        threads = [threading.Thread(target=workload, args=(i,), daemon=True)
                   for i in range(args.workers)]
        for thread in threads:
            thread.start()
        while time.time() - started < args.seconds:
            if proc.poll() is not None:
                server_exit_code = proc.returncode
                dead_at = time.time() - started
                break
            time.sleep(args.interval)
            try:
                status, body, _ = kit.get(port, "/metrics")
            except OSError:
                status, body = -1, b""
            if status != 200:
                with lock:
                    counters["errors"] += 1
                continue
            values, _, _, _ = kit.parse_metrics(body.decode("utf-8"))
            now = round(time.time(), 3)
            rss = float(values.get("llmopt_rss_mib", float("nan")))
            rss_peak = float(values.get("llmopt_rss_peak_mib", float("nan")))
            if rss == rss:
                rss_rows.append([now, round(rss, 3), round(rss_peak, 3)])
                samples["rss"].append((now, rss))
            used = float(values.get("llmopt_kv_used_blocks", float("nan")))
            cached = float(values.get("llmopt_kv_cached_blocks", float("nan")))
            free = float(values.get("llmopt_kv_free_blocks", float("nan")))
            diffs = float(values.get("llmopt_kv_reconcile_diffs_total", float("nan")))
            if used == used:
                kv_rows.append([now, int(used), int(cached) if cached == cached else -1,
                                int(free) if free == free else -1,
                                int(diffs) if diffs == diffs else -1])
                samples["used"].append((now, used))
                samples["cached"].append((now, cached))
                samples["reconcile"].append((now, diffs))
        stop.set()
        for thread in threads:
            thread.join(timeout=120)
        time.sleep(1.0)
        drain_ok = False
        drain_polls = 0
        for _ in range(40):
            status, body, _ = kit.get(port, "/metrics")
            if status == 200:
                values, _, _, _ = kit.parse_metrics(body.decode("utf-8"))
                used = float(values.get("llmopt_kv_used_blocks", -1))
                cached = float(values.get("llmopt_kv_cached_blocks", -1))
                if used == 0 and cached == 0:
                    drain_ok = True
                    break
            drain_polls += 1
            time.sleep(0.5)
    finally:
        stop.set()
        proc.terminate()
        try:
            proc.wait(timeout=40)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
        log_handle.close()

    write_csv(out_dir / "rss.csv", ["ts_unix", "rss_mib", "rss_peak_mib"], rss_rows)
    write_csv(out_dir / "kv_used.csv",
              ["ts_unix", "used_blocks", "cached_blocks", "free_blocks", "reconcile_diffs"],
              kv_rows)
    with (out_dir / "itl_samples.jsonl").open("w", encoding="utf-8") as handle:
        for row in itl_rows:
            handle.write(json.dumps({"ts_unix": row[0], "request_id": row[1],
                                     "itl_ms": row[2]}) + "\n")

    end_ts = started + args.seconds
    first = [v for ts, v in samples_itl if ts <= started + FIRST_WINDOW_S]
    last = [v for ts, v in samples_itl if ts >= end_ts - LAST_WINDOW_S]
    rss_first = [v for ts, v in samples["rss"] if ts <= started + FIRST_WINDOW_S]
    rss_last = [v for ts, v in samples["rss"] if ts >= end_ts - LAST_WINDOW_S]
    rss_delta = (mean(rss_last) - mean(rss_first)) if rss_first and rss_last else 0.0
    p99_first, p99_last = p99(first), p99(last)
    p99_ratio = (p99_last / p99_first) if p99_first > 0 else 0.0

    used_values = [v for _, v in samples["used"]]
    monotonic = len(used_values) > 2 and all(b >= a for a, b in zip(used_values, used_values[1:]))
    reconcile_max = max([v for _, v in samples["reconcile"]] or [0.0])
    report = {
        "seconds": args.seconds,
        "workers": args.workers,
        "ctx_tokens": args.ctx_tokens,
        "max_tokens": args.max_tokens,
        "requests": counters["requests"],
        "errors": counters["errors"],
        "server_exit_code": server_exit_code,
        "server_dead_after_s": round(dead_at, 1) if dead_at else None,
        "rss_first_mean_mib": round(mean(rss_first), 2),
        "rss_last_mean_mib": round(mean(rss_last), 2),
        "rss_delta_mib": round(rss_delta, 2),
        "rss_limit_mib": RSS_LIMIT_MIB,
        "itl_p99_first_ms": round(p99_first, 3),
        "itl_p99_last_ms": round(p99_last, 3),
        "itl_p99_ratio": round(p99_ratio, 3),
        "itl_p99_ratio_limit": P99_RATIO_LIMIT,
        "kv_used_monotonic": monotonic,
        "kv_drain_ok": drain_ok,
        "kv_reconcile_diffs_max": reconcile_max,
        "samples_rss": len(rss_rows),
        "samples_kv": len(kv_rows),
        "samples_itl": len(itl_rows),
        "verdict": {
            "rss": "pass" if rss_delta <= RSS_LIMIT_MIB else "fail",
            "itl_p99": "pass" if p99_ratio <= P99_RATIO_LIMIT else "fail",
            "kv_drain": "pass" if drain_ok and not monotonic and reconcile_max == 0 else "fail",
            "errors": "pass" if counters["errors"] == 0 else "fail",
            "server_alive": "pass" if server_exit_code is None else "fail",
        },
    }
    (out_dir / "soak_report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2), encoding="utf-8")

    print(json.dumps(report, indent=2))
    ok = (counters["errors"] == 0 and rss_delta <= RSS_LIMIT_MIB
          and p99_ratio <= P99_RATIO_LIMIT and drain_ok and not monotonic
          and reconcile_max == 0 and server_exit_code is None)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
