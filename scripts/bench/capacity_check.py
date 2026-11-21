#!/usr/bin/env python3
"""capacity_check.py — 用声明档位 + 本机真实可用显存/内存跑启动容量判据。

只验证 `模型体积 + KV + n_gpu_layers` 是否进得了预算，不加载权重、不产出吞吐。
输出 CSV 每行一档（ngl/ctx/P/kv），证据可与 `docs/架构审计与容量设计.md` 对照。

用法：
    python scripts/bench/capacity_check.py --server /root/llmopt-build/llmopt_server \
        --profile scripts/bench/capacity/qwen3-27b-dense-q4.json \
        --ngl 0,8,16,24,32 --out evidence/capacity_27b.csv
"""
from __future__ import annotations

import argparse
import csv
import os
import re
import subprocess
import sys
from pathlib import Path

PROFILE_LINE = re.compile(
    r"estimate:\s*model=(\d+)MiB\s+kv=(\d+)MiB\s+required=(\d+)MiB\s+free=(\d+)MiB"
    r"\s+safety=([0-9.]+)\s+decision=([A-Za-z]+)")
SPLIT_LINE = re.compile(
    r"estimate-split:\s*model_vram=(\d+)MiB\s+kv_vram=(\d+)MiB\s+host=(\d+)MiB")
CAPACITY_LINE = re.compile(r"capacity-only:\s*allow_start=(\d)")
PROFILE_HEAD = re.compile(r"capacity-profile:.*?name=(\S+)")

FIELDS = [
    "profile",
    "n_gpu_layers",
    "ctx_tokens",
    "max_concurrency",
    "kv_cache_type",
    "model_mib",
    "kv_mib",
    "required_mib",
    "free_mib",
    "model_vram_mib",
    "kv_vram_mib",
    "host_mib",
    "decision",
    "allow_start",
    "exit_code",
    "reason",
]


def parse_output(text: str) -> dict:
    """只从服务自己的 estimate/capacity-only 行取值；缺行就留空，不估算补齐。"""
    row: dict = {
        "profile": "",
        "model_mib": "",
        "kv_mib": "",
        "required_mib": "",
        "free_mib": "",
        "model_vram_mib": "",
        "kv_vram_mib": "",
        "host_mib": "",
        "decision": "",
        "allow_start": "",
        "reason": "",
    }
    if match := PROFILE_HEAD.search(text):
        row["profile"] = match.group(1)
    if match := PROFILE_LINE.search(text):
        row.update({
            "model_mib": int(match.group(1)),
            "kv_mib": int(match.group(2)),
            "required_mib": int(match.group(3)),
            "free_mib": int(match.group(4)),
            "decision": match.group(6),
        })
    if match := SPLIT_LINE.search(text):
        row.update({
            "model_vram_mib": int(match.group(1)),
            "kv_vram_mib": int(match.group(2)),
            "host_mib": int(match.group(3)),
        })
    if match := CAPACITY_LINE.search(text):
        row["allow_start"] = int(match.group(1))
    reasons = [line.split("gate: ", 1)[1].strip()
               for line in text.splitlines() if line.startswith("gate: ")]
    row["reason"] = "；".join(reasons)
    return row


def run_one(server: Path, profile: Path, ngl: int, ctx: int, concurrency: int,
            batch_tokens: int, kv_type: str, timeout_s: int = 120) -> dict:
    command = [
        str(server), "--model-profile", str(profile), "--backend", "stub", "--capacity-only",
        "--mode", "auto", "--n-gpu-layers", str(ngl), "--ctx-tokens", str(ctx),
        "--max-concurrency", str(concurrency), "--max-num-batched-tokens", str(batch_tokens),
        "--donor-slots", "0", "--disable-prefix-cache", "--kv-cache-type", kv_type,
        "--log-level", "warn",
    ]
    env = dict(os.environ)
    env["LLMOPT_ALLOW_STUB"] = "1"
    row = parse_output("")
    row.update({
        "n_gpu_layers": ngl,
        "ctx_tokens": ctx,
        "max_concurrency": concurrency,
        "kv_cache_type": kv_type,
    })
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout_s, env=env)
    except subprocess.TimeoutExpired as exc:
        row["exit_code"] = 124
        row["reason"] = f"超时 {timeout_s}s 未退出"
        partial = exc.output or exc.stderr or ""
        if isinstance(partial, bytes):
            partial = partial.decode("utf-8", "replace")
        for key, value in parse_output(partial).items():
            if value != "":
                row[key] = value
        return row
    except OSError as exc:
        row["exit_code"] = 1
        row["reason"] = f"启动失败: {exc}"
        return row
    output = result.stdout + result.stderr
    row.update(parse_output(output))
    row["exit_code"] = result.returncode
    return row


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="启动容量判据（不加载权重）")
    parser.add_argument("--server", required=True)
    parser.add_argument("--profile", required=True)
    parser.add_argument("--ngl", default="0,8,16,24,32")
    parser.add_argument("--ctx-tokens", type=int, default=1024)
    parser.add_argument("--max-concurrency", type=int, default=4)
    parser.add_argument("--max-num-batched-tokens", type=int, default=1024)
    parser.add_argument("--kv-cache-type", default="q8_0")
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)

    server = Path(args.server)
    profile = Path(args.profile)
    if not server.exists():
        print(f"服务端不存在: {server}", file=sys.stderr)
        return 3
    if not profile.exists():
        print(f"档位文件不存在: {profile}", file=sys.stderr)
        return 3

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    rows = []
    with out.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS)
        writer.writeheader()
        for ngl in [int(value) for value in args.ngl.split(",") if value.strip()]:
            row = run_one(server, profile, ngl, args.ctx_tokens, args.max_concurrency,
                          args.max_num_batched_tokens, args.kv_cache_type)
            rows.append(row)
            writer.writerow(row)
            handle.flush()
            print(f"[capacity] ngl={ngl:>3} decision={row['decision'] or 'FAIL'} "
                  f"allow={row['allow_start']} exit={row['exit_code']}")
    print(f"[capacity] 写入 {out}（{len(rows)} 行）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
