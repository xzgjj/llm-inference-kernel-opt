# soak.py — 6-G7：持续请求下 RSS 增长 <5%、无 5xx、服务保持 ready。
from __future__ import annotations

import argparse
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


def one_request(port: int, index: int) -> tuple[int, float]:
    """跑一条流式请求，返回 (状态码, 首个 token 延迟 ms)。"""
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=60)
    started = time.perf_counter()
    try:
        payload = json.dumps({"prompt": f"soak-{index}", "stream": True, "max_tokens": 16,
                              "seed": index % 97})
        conn.request("POST", "/v1/completions", payload, {"Content-Type": "application/json"})
        response = conn.getresponse()
        if response.status != 200:
            response.read()
            return response.status, 0.0
        first = 0.0
        while True:
            line = response.readline()
            if not line:
                break
            if line.startswith(b"data: [DONE]"):
                break
            if line.startswith(b"data: ") and not first:
                first = (time.perf_counter() - started) * 1000.0
        return 200, first
    finally:
        conn.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("server", type=Path)
    parser.add_argument("--seconds", type=int, default=1800)
    parser.add_argument("--interval", type=float, default=30.0)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--report", type=Path, default=None)
    args = parser.parse_args()

    workdir = Path(tempfile.mkdtemp(prefix="soak-"))
    model = workdir / "soak.gguf"
    kit.make_gguf(model)
    port = kit.free_port()
    env = dict(os.environ)
    env["LLMOPT_ALLOW_STUB"] = "1"
    proc = subprocess.Popen([str(args.server), "--model", str(model), "--host", "127.0.0.1",
                             "--port", str(port), "--backend", "stub", "--mode", "off"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env,
                            cwd=str(workdir))
    stop = threading.Event()
    counters = {"requests": 0, "errors": 0}
    lock = threading.Lock()
    rss: list[float] = []

    def workload(worker: int) -> None:
        index = worker
        while not stop.is_set():
            try:
                status, _ = one_request(port, index)
            except OSError:
                status = -1
            with lock:
                counters["requests"] += 1
                if status != 200:
                    counters["errors"] += 1
            index += args.workers
            time.sleep(0.02)

    try:
        kit.wait_ready(port, proc, timeout=60)
        threads = [threading.Thread(target=workload, args=(i,), daemon=True)
                   for i in range(args.workers)]
        for thread in threads:
            thread.start()
        start = time.time()
        baseline = None
        while time.time() - start < args.seconds:
            time.sleep(args.interval)
            status, body, _ = kit.get(port, "/metrics")
            if status != 200:
                counters["errors"] += 1
                continue
            samples, _, _, _ = kit.parse_metrics(body.decode("utf-8"))
            current = samples.get("llmopt_rss_mib", float("nan"))
            if current != current:
                continue
            rss.append(current)
            if baseline is None:
                baseline = current
        stop.set()
        for thread in threads:
            thread.join(timeout=70)
        growth = 0.0 if not rss or baseline in (None, 0) else (max(rss) - baseline) / baseline
        report = {
            "seconds": args.seconds,
            "requests": counters["requests"],
            "errors": counters["errors"],
            "rss_baseline_mib": baseline,
            "rss_max_mib": max(rss) if rss else None,
            "rss_growth_ratio": growth,
        }
        if args.report:
            args.report.write_text(json.dumps(report, indent=2), encoding="utf-8")
        assert baseline is not None, "未采到 llmopt_rss_mib"
        assert counters["errors"] == 0, f"出现非 200 响应：{counters['errors']}"
        assert growth < 0.05, f"RSS 增长 {growth:.2%} 超过 5%"
        print(f"soak ok: {json.dumps(report)}")
        return 0
    finally:
        stop.set()
        proc.terminate()
        try:
            proc.wait(timeout=40)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
