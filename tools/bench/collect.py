"""阶段 0 采集器入口：起 SUT、逐格并发压测、落 JSONL、聚合 CSV 与 manifest。

用法：
  python -m tools.bench.collect --config tools/bench/config/phase0_default.json
  python -m tools.bench.collect --config ... --out tools/bench/results/phase0/run-x
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import platform
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from . import aggregate, client, prompts, resources

REPO_ROOT = Path(__file__).resolve().parents[2]
HEALTH_TIMEOUT_S = 120
PDF_FIELDS = ("run_id", "concurrency_p", "n_requests", "ttft_p50_ms", "itl_p50_ms")


def _load_config(path: Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    required = ("model", "server", "ctx_tokens", "concurrency", "tiers", "max_tokens", "seed")
    missing = [key for key in required if key not in data]
    if missing:
        raise SystemExit(f"配置缺少字段: {missing}")
    return data


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _server_commit(binary: Path) -> str:
    """SUT 源码 commit：优先从二进制所在仓库读，失败记 unknown。"""
    source_root = Path(os.environ.get("LLMOPT_LLAMA_ROOT", "/root/llm-src/llama.cpp"))
    rc = subprocess.run(
        ["git", "-C", str(source_root), "rev-parse", "HEAD"],
        capture_output=True, text=True, check=False,
    )
    return rc.stdout.strip() if rc.returncode == 0 else f"binary-sha:{_sha256_file(binary)[:12]}"


def _run_side() -> str:
    try:
        release = Path("/proc/sys/kernel/osrelease").read_text(encoding="utf-8")
        return "wsl2" if "microsoft" in release.lower() else "linux"
    except OSError:
        return "windows"


def _wait_health(host: str, port: int, timeout_s: int, proc: subprocess.Popen) -> None:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"SUT 提前退出，码 {proc.returncode}")
        if client.health_ok(host, port):
            return
        time.sleep(1.0)
    raise RuntimeError(f"等待 /health 超时（{timeout_s}s）")


def _server_argv(cfg: dict, concurrency: int, port: int) -> list[str]:
    server = cfg["server"]
    ctx_total = int(cfg["ctx_tokens"]) * concurrency
    argv = [
        str(server["bin"]),
        "-m", str(cfg["model"]),
        "--host", str(cfg["host"]),
        "--port", str(port),
        "-c", str(ctx_total),
        "-np", str(concurrency),
        "-b", str(cfg.get("batch", 2048)),
        "-ub", str(cfg.get("ubatch", 512)),
        "-t", str(cfg.get("threads", 8)),
        "-ngl", str(cfg.get("n_gpu_layers", 0)),
        "--seed", str(cfg["seed"]),
    ]
    argv.extend(str(arg) for arg in server.get("extra_args", []))
    return argv


def _free_port(host: str, preferred: int) -> int:
    """优先用配置端口；被占用时向系统要一个空闲端口，保证不撞已有服务。"""
    if client.health_ok(host, preferred, 0.5) is False:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            try:
                sock.bind((host, preferred))
                return preferred
            except OSError:
                pass
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((host, 0))
        return int(sock.getsockname()[1])


def _payload(cfg: dict, text: str, max_tokens: int, seed: int) -> dict:
    sampling = cfg.get("sampling", {})
    return {
        "prompt": text,
        "n_predict": max_tokens,
        "stream": True,
        "temperature": float(sampling.get("temperature", 0.7)),
        "top_k": int(sampling.get("top_k", 40)),
        "top_p": float(sampling.get("top_p", 0.95)),
        "min_p": float(sampling.get("min_p", 0.05)),
        "repeat_penalty": float(sampling.get("repeat_penalty", 1.0)),
        "seed": seed,
    }


def _record_base(cfg: dict, row: dict, result, round_no: int, dropped: bool) -> dict:
    stats = result.stats
    itl = [
        stats.token_ts_ms[i + 1] - stats.token_ts_ms[i]
        for i in range(len(stats.token_ts_ms) - 1)
    ]
    return {
        "run_id": cfg["run_id"],
        "run_side": cfg["run_side"],
        "concurrency": int(cfg["concurrency_now"]),
        "tier": row["tier"],
        "round": round_no,
        "prompt_id": row["prompt_id"],
        "dropped": dropped,
        "status": result.status,
        "error_code": result.error_code,
        "t_enqueue_ms": stats.t_enqueue_ms,
        "t_first_ms": stats.t_first_ms,
        "t_end_ms": stats.t_end_ms,
        "ttft_ms": (stats.t_first_ms - stats.t_enqueue_ms) if stats.t_first_ms else None,
        "itl_ms": itl,
        "n_prompt": result.n_prompt,
        "n_gen": result.n_gen,
    }


def _run_one(cfg: dict, row: dict, round_no: int, dropped: bool, max_tokens: int) -> dict:
    result = client.run_stream(
        cfg["host"],
        cfg["port"],
        _payload(cfg, row["text"], max_tokens, int(cfg["seed"]) + int(row["prompt_id"].split("-")[-1])),
        row["n_prompt"],
        timeout_s=float(cfg.get("request_timeout_s", 120)),
    )
    return _record_base(cfg, row, result, round_no, dropped)


def _write_jsonl(path: Path, rows: list[dict]) -> None:
    with path.open("a", encoding="utf-8") as handle:
        for row in rows:
            handle.write(json.dumps(row, ensure_ascii=False, sort_keys=True) + "\n")


def _validate_prompts(cfg: dict, prompt_rows: list[dict]) -> None:
    """用 SUT 的 /tokenize 复核并回填 n_prompt；超限直接报错，不悄悄改样本。"""
    for row in prompt_rows:
        ids = client.tokenize(cfg["host"], cfg["port"], row["text"])
        if len(ids) > row["max_prompt_tokens"]:
            raise RuntimeError(
                f"{row['prompt_id']} 分词 {len(ids)} > 上限 {row['max_prompt_tokens']}"
            )
        row["n_prompt"] = len(ids)


def _warmup(cfg: dict, prompt_rows: list[dict], round_no: int, out: Path) -> None:
    """每个 prompt 档各预热一次：长档不得带着冷缓存进入第 1 轮。"""
    limit = int(cfg.get("warmup", 5))
    tiers = sorted({row["tier"] for row in prompt_rows})
    for tier in tiers:
        pool = [row for row in prompt_rows if row["tier"] == tier][:limit]
        workers = max(1, min(int(cfg["concurrency_now"]), len(pool) or 1))
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool_exec:
            futures = [
                pool_exec.submit(_run_one, cfg, row, round_no, True, 16) for row in pool
            ]
            rows = [f.result() for f in futures]
        _write_jsonl(out, rows)


def _run_cell(cfg: dict, tier_rows: list[dict], round_no: int, out: Path) -> None:
    concurrency = int(cfg["concurrency_now"])
    with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool_exec:
        futures = [
            pool_exec.submit(_run_one, cfg, row, round_no, False, int(cfg["max_tokens"]))
            for row in tier_rows
        ]
        rows = [f.result() for f in futures]
    _write_jsonl(out, rows)


def _launch(cfg: dict) -> tuple[subprocess.Popen, resources.ResourceSampler, Path]:
    concurrency = int(cfg["concurrency_now"])
    cfg["port"] = _free_port(cfg["host"], int(cfg["port"]))
    argv = _server_argv(cfg, concurrency, cfg["port"])
    log_path = Path(cfg["out_dir"]) / f"server-p{concurrency}.log"
    log_handle = log_path.open("w", encoding="utf-8")
    proc = subprocess.Popen(argv, stdout=log_handle, stderr=subprocess.STDOUT)
    cfg["_server_argv"] = argv
    try:
        _wait_health(cfg["host"], cfg["port"], HEALTH_TIMEOUT_S, proc)
    except Exception:
        proc.terminate()
        raise
    sampler = resources.ResourceSampler(proc.pid)
    sampler.start()
    return proc, sampler, log_path


def _stop(proc: subprocess.Popen, sampler: resources.ResourceSampler) -> resources.ResourcePeak:
    peak = sampler.stop()
    proc.terminate()
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    return peak


def _manifest(cfg: dict, prompt_rows: list[dict], peak: resources.ResourcePeak) -> dict:
    model_path = Path(cfg["model"])
    binary = Path(cfg["server"]["bin"])
    return {
        "run_id": cfg["run_id"],
        "seed": int(cfg["seed"]),
        "rounds": int(cfg["rounds"]),
        "warmup": int(cfg.get("warmup", 5)),
        "model_path": str(model_path),
        "model_sha256": _sha256_file(model_path),
        "sut_name": "llama.cpp server",
        "sut_commit": _server_commit(binary),
        "sut_build": _sha256_file(binary),
        "sut_argv": cfg.get("_server_argv", []),
        "env": {
            "run_side": cfg["run_side"],
            "platform": platform.platform(),
            "python": platform.python_version(),
            "cpu_count": os.cpu_count(),
            "hostname": platform.node(),
        },
        "prompt_set_hash": prompts.prompt_set_hash(prompt_rows),
        "config": {k: v for k, v in cfg.items() if not k.startswith("_") and k != "out_dir"},
        "resource_peak": {
            "rss_peak_bytes": peak.rss_peak_bytes,
            "vram_peak_bytes": peak.vram_peak_bytes,
            "samples": peak.samples,
        },
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="阶段 0 基线采集")
    parser.add_argument("--config", required=True)
    parser.add_argument("--out", default=None)
    parser.add_argument("--run-id", default=None)
    args = parser.parse_args(argv)

    cfg = _load_config(Path(args.config))
    cfg["run_id"] = args.run_id or time.strftime("p0-%Y%m%dT%H%M%SZ", time.gmtime())
    cfg["run_side"] = _run_side()
    out_dir = Path(args.out) if args.out else REPO_ROOT / "tools/bench/results/phase0" / cfg["run_id"]
    cfg["out_dir"] = str(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    tiers = [prompts.TierSpec(t["name"], int(t["max_prompt_tokens"]), int(t["n"])) for t in cfg["tiers"]]

    # 首轮启动一个临时 SUT 只为 tokenize；之后按并发档重启。
    cfg["concurrency_now"] = 1
    proc, sampler, _ = _launch(cfg)
    try:
        prompt_rows = prompts.build_prompt_set(
            int(cfg["seed"]), tiers, lambda text: len(client.tokenize(cfg["host"], cfg["port"], text))
        )
        _validate_prompts(cfg, prompt_rows)
        _write_jsonl(out_dir / "prompts_seed.jsonl", prompt_rows)
    finally:
        _stop(proc, sampler)
    prompt_set_hash = prompts.prompt_set_hash(prompt_rows)
    (out_dir / "prompt_set.json").write_text(
        json.dumps({"hash": prompt_set_hash, "prompts": prompt_rows}, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )

    records_path = out_dir / "requests.jsonl"
    records_path.unlink(missing_ok=True)
    peaks: list[resources.ResourcePeak] = []
    for concurrency in [int(value) for value in cfg["concurrency"]]:
        cfg["concurrency_now"] = concurrency
        proc, sampler, _ = _launch(cfg)
        try:
            _warmup(cfg, prompt_rows, 0, records_path)
            time.sleep(float(cfg.get("idle_between_cells_s", 10)))
            for round_no in range(1, int(cfg["rounds"]) + 1):
                for tier in tiers:
                    rows = [r for r in prompt_rows if r["tier"] == tier.name]
                    _run_cell(cfg, rows, round_no, records_path)
                time.sleep(float(cfg.get("idle_between_cells_s", 10)))
        finally:
            peaks.append(_stop(proc, sampler))
        print(f"[collect] P={concurrency} 完成")

    records = [json.loads(line) for line in records_path.read_text(encoding="utf-8").splitlines() if line]
    peak = resources.ResourcePeak(
        rss_peak_bytes=max((p.rss_peak_bytes for p in peaks), default=0),
        vram_peak_bytes=max((p.vram_peak_bytes for p in peaks), default=0),
        samples=sum(p.samples for p in peaks),
    )
    baseline, deviations = aggregate.aggregate(
        records,
        cfg["run_id"],
        [int(v) for v in cfg["concurrency"]],
        [t.name for t in tiers],
        int(cfg["rounds"]),
    )
    aggregate.write_csv(out_dir / "baseline.csv", baseline, aggregate.BASELINE_FIELDS)
    aggregate.write_csv(out_dir / "round_deviation.csv", deviations, aggregate.DEVIATION_FIELDS)
    manifest = _manifest(cfg, prompt_rows, peak)
    (out_dir / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(f"[collect] 行数 {len(records)}，输出 {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
