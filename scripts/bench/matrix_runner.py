#!/usr/bin/env python3
"""matrix_runner.py — 阶段 4 实验矩阵入口（4-G1 / 4-G2 / 4-G6）。

用法：
    python scripts/bench/matrix_runner.py --config scripts/bench/matrix/phase4.json \
        --out bench/results/phase4 --server build/llmopt_server.exe \
        --model bench/models/qwen2.5-0.5b-q4.gguf
    python scripts/bench/matrix_runner.py --config ... --smoke
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.bench import matrix  # noqa: E402

DEFAULT_CONFIG = ROOT / "scripts" / "bench" / "matrix" / "phase4.json"


def smoke_overrides() -> dict:
    """冒烟：1/2 并发、1 轮、每档 3 条，供集成测试在秒级跑完。"""
    return {
        "concurrency": [1, 2],
        "repeats": 1,
        "warmup_requests": 1,
        "max_tokens": 8,
        "idle_between_cells_s": 0.0,
        "backend": "stub",
        "tiers": [
            {"name": "short", "max_prompt_tokens": 64, "n": 3},
            {"name": "long", "max_prompt_tokens": 160, "n": 3},
        ],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="阶段 4 实验矩阵")
    parser.add_argument("--config", default=str(DEFAULT_CONFIG))
    parser.add_argument("--out", default=None)
    parser.add_argument("--run-id", default=None)
    parser.add_argument("--server", default=None, help="覆盖配置里的 server 路径")
    parser.add_argument("--model", default=None, help="覆盖配置里的 model 路径")
    parser.add_argument("--backend", default=None, help="stub / llama")
    parser.add_argument("--mixed", action="store_true", help="长短 1:1 混合（4-G6）")
    parser.add_argument("--smoke", action="store_true", help="小矩阵快速冒烟")
    args = parser.parse_args(argv)

    overrides: dict = {}
    if args.server:
        overrides["server"] = args.server
    if args.model:
        overrides["model"] = args.model
    if args.backend:
        overrides["backend"] = args.backend
    if args.mixed:
        overrides["mixed"] = True
    if args.smoke:
        overrides.update(smoke_overrides())

    cfg = matrix.load_config(Path(args.config), overrides)
    run_id = args.run_id or time.strftime("p4-%Y%m%dT%H%M%SZ", time.gmtime())
    out_dir = Path(args.out) if args.out else ROOT / "bench" / "results" / "phase4" / run_id
    return matrix.run_matrix(cfg, out_dir, run_id)


if __name__ == "__main__":
    raise SystemExit(main())
