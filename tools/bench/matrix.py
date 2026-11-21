"""阶段 4 实验矩阵：起 SUT、逐并发档发压、落 JSONL、聚合 SPEC §1.6 的 CSV 列。

矩阵配置用 JSON（项目不引 YAML 库，SPEC 已记录偏差）；档间重启，避免前档
KV/缓存污染后档。不可行档只记 `infeasible`/`failed` + reason，不中断后续档。
"""

from __future__ import annotations

import concurrent.futures
import csv
import hashlib
import json
import os
import platform
import re
import socket
import subprocess
import threading
import time
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from . import aggregate, client, prompts

REPO_ROOT = Path(__file__).resolve().parents[2]
HEALTH_TIMEOUT_S = 120
REPORT_FIELDS = [
    "concurrency_p",
    "prompt_tier",
    "status",
    "n_requests",
    "ttft_p50_ms",
    "ttft_p95_ms",
    "itl_p99_ms",
    "total_tok_s",
    "tok_accounting",
    "kv_internal_frag_ratio",
    "sched_cpu_ratio",
    "vram_peak_mib",
    "vram_peak_scope",
    "vram_device_peak_mib",
    "reason",
    "deviation_flag",
]
DEVIATION_FIELDS = [
    "concurrency_p",
    "prompt_tier",
    "ttft_p50_dev_pct",
    "itl_p50_dev_pct",
    "total_tok_s_dev_pct",
    "verdict",
]
MIN_SAMPLES = 30
TTFT_ITL_LIMIT = 0.10
THROUGHPUT_LIMIT = 0.07
# llama.cpp 在加载/预留时打印各设备缓冲区；CUDA0 三项之和就是本进程在卡上的常驻。
# WSL 里 `nvidia-smi --query-compute-apps` 拿不到 pid 归属，这组日志是唯一可归因的显存证据。
_CUDA_BUFFER_PATTERNS = {
    "model": re.compile(r"CUDA0\s+model buffer size\s*=\s*([0-9.]+)\s*MiB"),
    "kv": re.compile(r"CUDA0\s+KV buffer size\s*=\s*([0-9.]+)\s*MiB"),
    "compute": re.compile(r"CUDA0\s+compute buffer size\s*=\s*([0-9.]+)\s*MiB"),
}
_HOST_BUFFER_PATTERNS = {
    "model": re.compile(r"CPU_Mapped\s+model buffer size\s*=\s*([0-9.]+)\s*MiB"),
    "kv": re.compile(r"CPU\s+KV buffer size\s*=\s*([0-9.]+)\s*MiB"),
    "compute": re.compile(r"CUDA_Host\s+compute buffer size\s*=\s*([0-9.]+)\s*MiB"),
}


class StartupError(RuntimeError):
    """SUT 未就绪或提前退出；由调用方记 infeasible，而不是崩矩阵。"""


@dataclass(frozen=True)
class Tier:
    name: str
    max_prompt_tokens: int
    n: int


@dataclass
class MatrixConfig:
    server: Path
    model: Path
    backend: str
    concurrency: list[int]
    tiers: list[Tier]
    repeats: int
    warmup_requests: int
    max_tokens: int
    ctx_tokens: int
    max_num_batched_tokens: int
    queue_capacity: int
    tier_timeout_ms: int
    request_timeout_s: float
    seed: int
    host: str
    mixed: bool
    idle_between_cells_s: float
    # 新增项给默认值，保证离线重建（report.py）只凭 manifest 直构时不必补齐。
    n_gpu_layers: int = 0
    threads: int = 0
    kv_cache_type: str = "f16"
    donor_slots: int = 4
    disable_prefix_cache: bool = False
    chunk_size: int = 128
    raw: dict[str, Any] = field(default_factory=dict)

    def config_hash(self) -> str:
        payload = {key: value for key, value in self.raw.items() if not key.startswith("_")}
        text = json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
        return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _require(data: dict, key: str) -> Any:
    if key not in data:
        raise SystemExit(f"矩阵配置缺少字段: {key}")
    return data[key]


def load_config(path: Path, overrides: dict[str, Any] | None = None) -> MatrixConfig:
    data = json.loads(path.read_text(encoding="utf-8"))
    if overrides:
        data.update({key: value for key, value in overrides.items() if value is not None})
    tiers = [
        Tier(str(item["name"]), int(item["max_prompt_tokens"]), int(item["n"]))
        for item in _require(data, "tiers")
    ]
    if not tiers:
        raise SystemExit("tiers 不能为空")
    repeats = int(data.get("repeats", 2))
    if repeats < 1:
        raise SystemExit("repeats 必须 >= 1")
    concurrency = [int(value) for value in _require(data, "concurrency")]
    if not concurrency or any(value < 1 for value in concurrency):
        raise SystemExit("concurrency 必须是正整数列表")
    return MatrixConfig(
        server=Path(str(_require(data, "server"))),
        model=Path(str(_require(data, "model"))),
        backend=str(data.get("backend", "stub")),
        concurrency=concurrency,
        tiers=tiers,
        repeats=repeats,
        warmup_requests=int(data.get("warmup_requests", 5)),
        max_tokens=int(data.get("max_tokens", 64)),
        ctx_tokens=int(data.get("ctx_tokens", 2048)),
        max_num_batched_tokens=int(data.get("max_num_batched_tokens", 2048)),
        queue_capacity=int(data.get("queue_capacity", 64)),
        tier_timeout_ms=int(data.get("tier_timeout_ms", 600000)),
        request_timeout_s=float(data.get("request_timeout_s", 120)),
        seed=int(data.get("seed", 7)),
        host=str(data.get("host", "127.0.0.1")),
        mixed=bool(data.get("mixed", False)),
        idle_between_cells_s=float(data.get("idle_between_cells_s", 1.0)),
        n_gpu_layers=int(data.get("n_gpu_layers", 0)),
        threads=int(data.get("threads", 0)),
        kv_cache_type=str(data.get("kv_cache_type", "f16")),
        donor_slots=int(data.get("donor_slots", 4)),
        disable_prefix_cache=bool(data.get("disable_prefix_cache", False)),
        chunk_size=int(data.get("chunk_size", 128)),
        raw=data,
    )


def build_prompts(cfg: MatrixConfig) -> list[dict]:
    """token 计数用字符估算桩：只决定档位长度，不参与延迟统计。"""
    rows = prompts.build_prompt_set(
        cfg.seed, [Tier(tier.name, tier.max_prompt_tokens, tier.n) for tier in cfg.tiers],
        prompts.char_token_count,
    )
    for row in rows:
        row["n_prompt"] = prompts.char_token_count(row["text"])
    return rows


def free_port(host: str) -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((host, 0))
        return int(sock.getsockname()[1])


def server_argv(cfg: MatrixConfig, concurrency: int, port: int,
                step_trace: Path | None = None) -> list[str]:
    argv = [
        str(cfg.server),
        "--model", str(cfg.model),
        "--host", cfg.host,
        "--port", str(port),
        "--backend", cfg.backend,
        "--mode", "off",
        "--ctx-tokens", str(cfg.ctx_tokens),
        "--max-concurrency", str(concurrency),
        "--queue-capacity", str(cfg.queue_capacity),
        "--max-num-batched-tokens", str(cfg.max_num_batched_tokens),
        "--chunk-size", str(cfg.chunk_size),
    ]
    if cfg.n_gpu_layers > 0:
        argv.extend(["--n-gpu-layers", str(cfg.n_gpu_layers)])
    if cfg.threads > 0:
        argv.extend(["--threads", str(cfg.threads)])
    if cfg.kv_cache_type != "f16":
        argv.extend(["--kv-cache-type", cfg.kv_cache_type])
    # 始终显式写 donor：服务默认是 4，不能靠“省略即默认”让 manifest 与实跑不一致。
    argv.extend(["--donor-slots", str(cfg.donor_slots)])
    if cfg.disable_prefix_cache:
        argv.append("--disable-prefix-cache")
    if step_trace is not None:
        argv.extend(["--step-trace", str(step_trace)])
    return argv


def _wait_ready(host: str, port: int, proc: subprocess.Popen, timeout_s: int = HEALTH_TIMEOUT_S) -> None:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if proc.poll() is not None:
            raise StartupError(f"SUT 提前退出 rc={proc.returncode}")
        if client.health_ok(host, port, 1.0, "/healthz"):
            return
        time.sleep(0.2)
    raise StartupError(f"等待 /healthz 超时（{timeout_s}s）")


def launch(cfg: MatrixConfig, concurrency: int, out_dir: Path) -> tuple[subprocess.Popen, Any]:
    port = free_port(cfg.host)
    trace_path = out_dir / f"steps-p{concurrency}.jsonl"
    trace_path.unlink(missing_ok=True)
    argv = server_argv(cfg, concurrency, port, trace_path)
    log_path = out_dir / f"server-p{concurrency}.log"
    log_handle = log_path.open("w", encoding="utf-8")
    env = dict(os.environ)
    if cfg.backend == "stub":
        env["LLMOPT_ALLOW_STUB"] = "1"
    proc = subprocess.Popen(argv, stdout=log_handle, stderr=subprocess.STDOUT, env=env)
    try:
        _wait_ready(cfg.host, port, proc)
    except StartupError as exc:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        finally:
            log_handle.close()
        raise StartupError(f"{exc}；日志 {log_path}") from exc
    from .resources import ResourceSampler

    sampler = ResourceSampler(proc.pid, interval_s=0.5)
    sampler.start()
    return proc, (port, sampler, log_handle)


def stop(proc: subprocess.Popen, handle: Any) -> Any:
    _, sampler, log_handle = handle
    resource_peak = sampler.stop()
    proc.terminate()
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    log_handle.close()
    return resource_peak


def payload(cfg: MatrixConfig, text: str, max_tokens: int, seed: int) -> dict:
    sampling = cfg.raw.get("sampling", {})
    return {
        "prompt": text,
        "max_tokens": max_tokens,
        "stream": True,
        "temperature": float(sampling.get("temperature", 0.0)),
        "top_k": int(sampling.get("top_k", 1)),
        "top_p": float(sampling.get("top_p", 1.0)),
        "min_p": float(sampling.get("min_p", 0.0)),
        "repeat_penalty": float(sampling.get("repeat_penalty", 1.0)),
        "seed": seed,
    }


def run_one(cfg: MatrixConfig, port: int, concurrency: int, row: dict, round_no: int,
            dropped: bool, max_tokens: int, timeout_s: float | None = None) -> dict:
    prompt_seed = cfg.seed + int(row["prompt_id"].split("-")[-1])
    result = client.run_stream(
        cfg.host, port, payload(cfg, row["text"], max_tokens, prompt_seed),
        int(row["n_prompt"]), timeout_s=timeout_s or cfg.request_timeout_s, path="/v1/completions",
    )
    stats = result.stats
    itl = [
        stats.token_ts_ms[index + 1] - stats.token_ts_ms[index]
        for index in range(len(stats.token_ts_ms) - 1)
    ]
    return {
        "concurrency": concurrency,
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


def _timeout_record(cfg: MatrixConfig, concurrency: int, row: dict, round_no: int) -> dict:
    return {
        "concurrency": concurrency,
        "tier": row["tier"],
        "round": round_no,
        "prompt_id": row["prompt_id"],
        "dropped": False,
        "status": "error",
        "error_code": "tier_timeout",
        "t_enqueue_ms": 0.0,
        "t_first_ms": None,
        "t_end_ms": None,
        "ttft_ms": None,
        "itl_ms": [],
        "n_prompt": row["n_prompt"],
        "n_gen": 0,
    }


def _run_batch(cfg: MatrixConfig, port: int, concurrency: int, rows: list[dict], round_no: int,
               dropped: bool, max_tokens: int) -> list[dict]:
    workers = max(1, min(concurrency, len(rows)))
    # 单请求超时同时受档预算约束，避免一档拖过墙钟上限。
    request_timeout = min(cfg.request_timeout_s, cfg.tier_timeout_ms / 1000.0)
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
        futures = {
            executor.submit(run_one, cfg, port, concurrency, row, round_no, dropped, max_tokens,
                            request_timeout): row
            for row in rows
        }
        done, pending = concurrent.futures.wait(
            futures, timeout=cfg.tier_timeout_ms / 1000.0
        )
    for future in pending:
        future.cancel()
    records: list[dict] = []
    for future, row in futures.items():
        if future in done and not future.cancelled():
            records.append(future.result())
        else:
            records.append(_timeout_record(cfg, concurrency, row, round_no))
    return records


def write_jsonl(path: Path, rows: list[dict]) -> None:
    with path.open("a", encoding="utf-8") as handle:
        for row in rows:
            handle.write(json.dumps(row, ensure_ascii=False, sort_keys=True) + "\n")


def _warmup(cfg: MatrixConfig, port: int, concurrency: int, prompt_rows: list[dict],
            out_path: Path) -> None:
    for tier in cfg.tiers:
        pool = [row for row in prompt_rows if row["tier"] == tier.name][: cfg.warmup_requests]
        if not pool:
            continue
        write_jsonl(out_path,
                    _run_batch(cfg, port, concurrency, [dict(row) for row in pool], 0, True, 16))


def fetch_metrics(cfg: MatrixConfig, port: int) -> str:
    url = f"http://{cfg.host}:{port}/metrics"
    try:
        with urllib.request.urlopen(url, timeout=10) as response:
            return response.read().decode("utf-8", "replace")
    except OSError:
        return ""


class _LiveMetricSampler:
    """压测窗口内轮询 /metrics，保存 K-4/K-5 的峰值。

    格结束时的快照是在请求排空之后取的，gauge 已经被重置为 0；直接用它填
    `kv_internal_frag_ratio` 会得到一列全 0。这里只提升精度，不伪造数据：
    最终 CSV 仍写真实快照与采样峰值的较大者，原始 `.prom` 保持原样。
    """

    def __init__(self, cfg: MatrixConfig, port: int, interval_s: float = 0.5) -> None:
        self._cfg = cfg
        self._port = port
        self._interval_s = interval_s
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self.kv = 0.0
        self.sched = 0.0
        self.device_vram = 0.0
        self.kv_used = 0

    def start(self) -> None:
        self._thread = threading.Thread(target=self._loop, name="metrics-live", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)

    def _loop(self) -> None:
        while not self._stop.wait(self._interval_s):
            text = fetch_metrics(self._cfg, self._port)
            if not text:
                continue
            kv = parse_named(text, "llmopt_kv_internal_frag_ratio")
            sched = parse_named(text, "llmopt_sched_cpu_ratio")
            device = parse_named(text, "llmopt_vram_device_mib")
            kv_used = parse_named(text, "llmopt_kv_used_blocks")
            if kv is not None:
                self.kv = max(self.kv, kv)
            if sched is not None:
                self.sched = max(self.sched, sched)
            if device is not None and device == device:  # NaN 不参与取峰
                self.device_vram = max(self.device_vram, device)
            if kv_used is not None:
                self.kv_used = max(self.kv_used, int(kv_used))


def read_new_trace(path: Path, start_line: int) -> tuple[list[dict], int]:
    """读 step trace 的新增行；返回 (本轮记录, 新行号)，避免每轮重读全文件。"""
    if not path.exists():
        return [], start_line
    lines = path.read_text(encoding="utf-8").splitlines()
    fresh = []
    for line in lines[start_line:]:
        if not line.strip():
            continue
        try:
            fresh.append(json.loads(line))
        except json.JSONDecodeError:
            # 进程被强杀时最后一行可能截断；丢掉半行，不伪造样本。
            continue
    return fresh, len(lines)


def parse_vram_budgets(text: str) -> dict:
    """从 llama.cpp 日志提取 CUDA0/Host 缓冲区；CUDA0 之和即本进程显存占用。

    返回的 `vram_peak_scope` 只三种：`buffers`（model+KV+compute 齐全）、
    `buffers_partial`（加载中断或缺 compute）、`unknown`（无 CUDA 日志）。
    """
    cuda = {name: float(match.group(1)) for name, pattern in _CUDA_BUFFER_PATTERNS.items()
            if (match := pattern.search(text))}
    host = {name: float(match.group(1)) for name, pattern in _HOST_BUFFER_PATTERNS.items()
            if (match := pattern.search(text))}
    if not cuda:
        return {
            "vram_peak_mib": 0.0,
            "vram_peak_scope": "unknown",
            "cuda0_model_mib": 0.0,
            "cuda0_kv_mib": 0.0,
            "cuda0_compute_mib": 0.0,
            "host_model_mib": 0.0,
            "host_kv_mib": 0.0,
            "host_compute_mib": 0.0,
        }
    complete = {"model", "kv", "compute"} <= set(cuda)
    return {
        "vram_peak_mib": round(sum(cuda.values()), 3),
        "vram_peak_scope": "buffers" if complete else "buffers_partial",
        "cuda0_model_mib": round(cuda.get("model", 0.0), 3),
        "cuda0_kv_mib": round(cuda.get("kv", 0.0), 3),
        "cuda0_compute_mib": round(cuda.get("compute", 0.0), 3),
        "host_model_mib": round(host.get("model", 0.0), 3),
        "host_kv_mib": round(host.get("kv", 0.0), 3),
        "host_compute_mib": round(host.get("compute", 0.0), 3),
    }


def read_vram_budgets(path: Path) -> dict:
    if not path.exists():
        return parse_vram_budgets("")
    return parse_vram_budgets(path.read_text(encoding="utf-8", errors="replace"))


def step_stats(records: list[dict]) -> dict:
    steps = [int(record.get("ns", 0)) / 1e6 for record in records]
    return {
        "steps_total": sum(int(record.get("steps", 0)) for record in records),
        "step_ns_p50_ms": aggregate.percentile(steps, 0.50) if steps else None,
        "step_ns_p99_ms": aggregate.percentile(steps, 0.99) if steps else None,
    }


def parse_named(text: str, name: str) -> float | None:
    """取同名 gauge/counter 的最后一组样本；带标签的 family 汇总。"""
    value: float | None = None
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        metric, _, raw = line.rpartition(" ")
        if metric == name or metric.startswith(name + "{"):
            try:
                value = float(raw)
            except ValueError:
                continue
    return value


def _cell_round(records: list[dict], concurrency: int, tier: str, round_no: int,
                metrics: dict[str, float | None]) -> dict:
    subset = [
        record for record in records
        if record.get("concurrency") == concurrency
        and record.get("tier") == tier
        and record.get("round") == round_no
        and not record.get("dropped")
    ]
    valid = [record for record in subset if record.get("status") == "ok"
             and record.get("ttft_ms") is not None]
    status = "ok"
    if not valid:
        status = "infeasible" if subset and all(r.get("status") == "infeasible" for r in subset) else "failed"
    reason = ""
    if not valid:
        reason = subset[0].get("error_code") or "no_records" if subset else "no_records"
    ttfts = [float(record["ttft_ms"]) for record in valid]
    itls: list[float] = []
    for record in valid:
        itls.extend(float(value) for value in record.get("itl_ms", []))
    wall_s = 1e-9
    total_tok_s = None
    tok_accounting = "char_estimate"
    if valid:
        start = min(float(record["t_enqueue_ms"]) for record in valid)
        end = max(float(record["t_end_ms"]) for record in valid)
        wall_s = max((end - start) / 1000.0, 1e-9)
        gen_tokens = sum(int(record.get("n_gen", 0)) for record in valid)
        # 服务端 PrefillTokens 只计真正算过的 token；前缀命中部分要加回才是逻辑 prompt 长度。
        # 字符估算对中文 BPE 会高估 ~2.6×，两者都不能直接用字符数。
        prefill_tokens = metrics.get("prefill_tokens")
        prefix_hit_tokens = metrics.get("prefix_hit_tokens") or 0.0
        if prefill_tokens is not None and prefill_tokens + prefix_hit_tokens > 0:
            tokens = float(prefill_tokens) + float(prefix_hit_tokens) + gen_tokens
            tok_accounting = "prompt_metrics"
        else:
            tokens = sum(int(record.get("n_prompt", 0)) + int(record.get("n_gen", 0))
                        for record in valid)
        total_tok_s = tokens / wall_s
    return {
        "concurrency": concurrency,
        "tier": tier,
        "round": round_no,
        "status": status,
        "n_requests": len(valid),
        "ttft_p50_ms": aggregate.percentile(ttfts, 0.50) if ttfts else None,
        "ttft_p95_ms": aggregate.percentile(ttfts, 0.95) if ttfts else None,
        "itl_p50_ms": aggregate.percentile(itls, 0.50) if itls else None,
        "itl_p99_ms": aggregate.percentile(itls, 0.99) if itls else None,
        "total_tok_s": total_tok_s,
        "tok_accounting": tok_accounting,
        "kv_internal_frag_ratio": metrics.get("kv"),
        "sched_cpu_ratio": metrics.get("sched"),
        "reason": reason,
    }


def _pct_dev(first: float | None, second: float | None) -> float | None:
    if first is None or second is None:
        return None
    base = max(abs(first), abs(second))
    return None if base == 0.0 else abs(second - first) / base


def _median_round(cells: list[dict]) -> dict:
    ok = [cell for cell in cells if cell["status"] == "ok" and cell["total_tok_s"] is not None]
    if not ok:
        return cells[-1]
    ordered = sorted(ok, key=lambda cell: cell["total_tok_s"])
    return ordered[len(ordered) // 2]


def _deviation_flag(cfg: MatrixConfig, cells: list[dict]) -> tuple[str, dict[str, float | None]]:
    if len(cells) < 2:
        return "na", {"ttft": None, "itl": None, "total": None}
    first, second = cells[0], cells[1]
    if first["n_requests"] < MIN_SAMPLES or second["n_requests"] < MIN_SAMPLES:
        return "insufficient", {"ttft": None, "itl": None, "total": None}
    dev = {
        "ttft": _pct_dev(first["ttft_p50_ms"], second["ttft_p50_ms"]),
        "itl": _pct_dev(first["itl_p50_ms"], second["itl_p50_ms"]),
        "total": _pct_dev(first["total_tok_s"], second["total_tok_s"]),
    }
    if any(value is None for value in dev.values()):
        return "insufficient", dev
    assert dev["ttft"] is not None and dev["itl"] is not None and dev["total"] is not None
    if dev["ttft"] <= TTFT_ITL_LIMIT and dev["itl"] <= TTFT_ITL_LIMIT and dev["total"] <= THROUGHPUT_LIMIT:
        return "ok", dev
    return "high", dev


def summarize(cfg: MatrixConfig, records: list[dict],
              metrics_by_round: dict[tuple[int, int], dict[str, float | None]],
              vram_by_concurrency: dict[int, dict] | None = None) -> tuple[list[dict], list[dict]]:
    report: list[dict] = []
    deviations: list[dict] = []
    vram_by_concurrency = vram_by_concurrency or {}
    for concurrency in cfg.concurrency:
        vram = vram_by_concurrency.get(concurrency, parse_vram_budgets(""))
        device_peak = max(
            (float(metrics_by_round.get((concurrency, round_no), {}).get("device") or 0.0)
             for round_no in range(1, cfg.repeats + 1)),
            default=0.0,
        )
        for tier in cfg.tiers:
            cells = [
                _cell_round(records, concurrency, tier.name, round_no,
                            metrics_by_round.get((concurrency, round_no), {}))
                for round_no in range(1, cfg.repeats + 1)
            ]
            selected = _median_round(cells)
            flag, dev = _deviation_flag(cfg, cells)
            report.append({
                "concurrency_p": concurrency,
                "prompt_tier": tier.name,
                "status": selected["status"],
                "n_requests": selected["n_requests"],
                "ttft_p50_ms": selected["ttft_p50_ms"],
                "ttft_p95_ms": selected["ttft_p95_ms"],
                "itl_p99_ms": selected["itl_p99_ms"],
                "total_tok_s": selected["total_tok_s"],
                "tok_accounting": selected["tok_accounting"],
                "kv_internal_frag_ratio": selected["kv_internal_frag_ratio"],
                "sched_cpu_ratio": selected["sched_cpu_ratio"],
                "vram_peak_mib": vram.get("vram_peak_mib", 0.0),
                "vram_peak_scope": vram.get("vram_peak_scope", "unknown"),
                "vram_device_peak_mib": round(device_peak, 3),
                "reason": selected["reason"] or "na",
                "deviation_flag": flag,
            })
            deviations.append({
                "concurrency_p": concurrency,
                "prompt_tier": tier.name,
                "ttft_p50_dev_pct": _fmt_pct(dev["ttft"]),
                "itl_p50_dev_pct": _fmt_pct(dev["itl"]),
                "total_tok_s_dev_pct": _fmt_pct(dev["total"]),
                "verdict": flag,
            })
    return report, deviations


def _fmt_pct(value: float | None) -> float | str:
    return "" if value is None else round(value * 100.0, 3)


RATIO_FIELDS = {"kv_internal_frag_ratio", "sched_cpu_ratio"}
_METRIC_PRECISION = 3


def _fmt(value: float | None, digits: int = _METRIC_PRECISION) -> float | str:
    return "" if value is None else round(value, digits)


def write_report(path: Path, rows: list[dict], fields: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({
                key: _fmt(value, 6) if key in RATIO_FIELDS and isinstance(value, float)
                else _fmt(value) if isinstance(value, float) else value
                for key, value in row.items()
            })


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _git_commit() -> str:
    result = subprocess.run(
        ["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"],
        capture_output=True, text=True, check=False,
    )
    return result.stdout.strip() if result.returncode == 0 else "unknown"


def _peak_dict(peak: Any) -> dict:
    return {
        "rss_peak_bytes": int(getattr(peak, "rss_peak_bytes", 0)),
        "vram_peak_bytes": int(getattr(peak, "vram_peak_bytes", 0)),
        "vram_known": bool(getattr(peak, "vram_known", False)),
        "samples": int(getattr(peak, "samples", 0)),
    }


def _infeasible_records(cfg: MatrixConfig, concurrency: int, reason: str) -> list[dict]:
    records: list[dict] = []
    for tier in cfg.tiers:
        for round_no in range(1, cfg.repeats + 1):
            records.append({
                "concurrency": concurrency,
                "tier": tier.name,
                "round": round_no,
                "prompt_id": f"{tier.name}-infeasible",
                "dropped": False,
                "status": "infeasible",
                "error_code": reason,
                "t_enqueue_ms": 0.0,
                "t_first_ms": None,
                "t_end_ms": None,
                "ttft_ms": None,
                "itl_ms": [],
                "n_prompt": 0,
                "n_gen": 0,
            })
    return records


def _run_round(cfg: MatrixConfig, port: int, concurrency: int, prompt_rows: list[dict],
               round_no: int, out_path: Path) -> None:
    if cfg.mixed:
        short = [row for row in prompt_rows if row["tier"] == cfg.tiers[0].name]
        long = [row for row in prompt_rows if row["tier"] == cfg.tiers[-1].name]
        count = min(len(short), len(long))
        batch = short[:count] + long[:count]
        write_jsonl(out_path, _run_batch(cfg, port, concurrency, batch, round_no, False,
                                        cfg.max_tokens))
        return
    for tier in cfg.tiers:
        pool = [dict(row) for row in prompt_rows if row["tier"] == tier.name]
        write_jsonl(out_path, _run_batch(cfg, port, concurrency, pool, round_no, False,
                                        cfg.max_tokens))


def run_matrix(cfg: MatrixConfig, out_dir: Path, run_id: str) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    prompt_rows = build_prompts(cfg)
    (out_dir / "prompt_set.json").write_text(
        json.dumps({"hash": prompts.prompt_set_hash(prompt_rows), "prompts": prompt_rows},
                   ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    records_path = out_dir / "requests.jsonl"
    records_path.unlink(missing_ok=True)
    metrics_by_round: dict[tuple[int, int], dict[str, float | None]] = {}
    peaks: dict[int, dict] = {}
    vram_by_p: dict[int, dict] = {}
    step_rows: list[dict] = []
    for concurrency in cfg.concurrency:
        try:
            proc, handle = launch(cfg, concurrency, out_dir)
        except StartupError as exc:
            print(f"[matrix] P={concurrency} infeasible: {exc}")
            write_jsonl(records_path, _infeasible_records(cfg, concurrency, "startup_failed"))
            continue
        try:
            _warmup(cfg, handle[0], concurrency, prompt_rows, records_path)
            for round_no in range(1, cfg.repeats + 1):
                live = _LiveMetricSampler(cfg, handle[0])
                live.start()
                before_text = fetch_metrics(cfg, handle[0])
                prefill_before = parse_named(before_text, "llmopt_prefill_tokens_total") or 0.0
                prefix_hit_before = parse_named(
                    before_text, "llmopt_prefix_hit_tokens_total") or 0.0
                try:
                    _run_round(cfg, handle[0], concurrency, prompt_rows, round_no, records_path)
                finally:
                    live.stop()
                metrics_text = fetch_metrics(cfg, handle[0])
                prefill_after = parse_named(metrics_text, "llmopt_prefill_tokens_total") or 0.0
                prefix_hit_after = parse_named(
                    metrics_text, "llmopt_prefix_hit_tokens_total") or 0.0
                prefill_round = max(prefill_after - prefill_before, 0.0)
                prefix_hit_round = max(prefix_hit_after - prefix_hit_before, 0.0)
                (out_dir / f"metrics_p{concurrency}_r{round_no}.prom").write_text(
                    metrics_text, encoding="utf-8"
                )
                metrics_by_round[(concurrency, round_no)] = {
                    "kv": max(parse_named(metrics_text, "llmopt_kv_internal_frag_ratio") or 0.0,
                              live.kv),
                    "sched": max(parse_named(metrics_text, "llmopt_sched_cpu_ratio") or 0.0,
                                 live.sched),
                    "device": live.device_vram,
                    "kv_used": live.kv_used,
                    "kv_used_peak": parse_named(metrics_text, "llmopt_kv_used_blocks_peak"),
                    "preempt_admission": parse_named(
                        metrics_text, 'llmopt_preemptions_total{reason="admission"}'),
                    "preempt_chunk": parse_named(
                        metrics_text, 'llmopt_preemptions_total{reason="chunk_plan"}'),
                    "preempt_cap_errors": parse_named(metrics_text,
                                                      "llmopt_preempt_cap_errors_total"),
                    "chunk_deferred": parse_named(metrics_text, "llmopt_chunk_deferred_total"),
                    "chunk_steps": parse_named(metrics_text, "llmopt_chunk_prefill_steps_total"),
                    "prefill_tokens": prefill_round,
                    "prefix_hit_tokens": prefix_hit_round,
                }
                time.sleep(cfg.idle_between_cells_s)
        finally:
            peaks[concurrency] = _peak_dict(stop(proc, handle))
            vram_by_p[concurrency] = read_vram_budgets(out_dir / f"server-p{concurrency}.log")
            trace_lines = []
            trace_path = out_dir / f"steps-p{concurrency}.jsonl"
            if trace_path.exists():
                trace_lines, _ = read_new_trace(trace_path, 0)
            stats = step_stats(trace_lines)
            step_rows.append({"concurrency_p": concurrency, **stats})
        print(f"[matrix] P={concurrency} 完成")

    records = [json.loads(line) for line in records_path.read_text(encoding="utf-8").splitlines() if line]
    report, deviations = summarize(cfg, records, metrics_by_round, vram_by_p)
    write_report(out_dir / "matrix_report.csv", report, REPORT_FIELDS)
    write_report(out_dir / "deviation.csv", deviations, DEVIATION_FIELDS)
    write_report(out_dir / "step_summary.csv", step_rows,
                 ["concurrency_p", "steps_total", "step_ns_p50_ms", "step_ns_p99_ms"])
    manifest = {
        "run_id": run_id,
        "git_commit": _git_commit(),
        "seed": cfg.seed,
        "config_hash": cfg.config_hash(),
        "model_path": str(cfg.model),
        "model_sha256": _sha256(cfg.model) if cfg.model.exists() else "missing",
        "server_path": str(cfg.server),
        "server_sha256": _sha256(cfg.server) if cfg.server.exists() else "missing",
        "backend": cfg.backend,
        "rounds": cfg.repeats,
        "concurrency": cfg.concurrency,
        "tiers": [tier.__dict__ for tier in cfg.tiers],
        "prompt_set_hash": prompts.prompt_set_hash(prompt_rows),
        "thresholds": {"ttft_itl": TTFT_ITL_LIMIT, "throughput": THROUGHPUT_LIMIT},
        "argv": server_argv(cfg, cfg.concurrency[0], 0),
        "resource_peak": peaks,
        "vram_buffers": vram_by_p,
        "cell_metrics": {
            f"p{concurrency}": {
                f"r{round_no}": metrics_by_round.get((concurrency, round_no), {})
                for round_no in range(1, cfg.repeats + 1)
            }
            for concurrency in cfg.concurrency
        },
        "env": {
            "platform": platform.platform(),
            "python": platform.python_version(),
            "cpu_count": os.cpu_count(),
            "hostname": platform.node(),
        },
    }
    (out_dir / "repro_manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    from . import plot
    plot.plot_directory(out_dir)
    print(f"[matrix] 行数 {len(records)}，报告行 {len(report)}，输出 {out_dir}")
    expected = len(cfg.concurrency) * len(cfg.tiers)
    return 0 if len(report) == expected else 2

