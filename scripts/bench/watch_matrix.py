#!/usr/bin/env python3
"""watch_matrix.py — 轮询矩阵输出目录，给出可 tail 的进度行。

用法：
    python3 scripts/bench/watch_matrix.py --out bench/results/phase8b_long \
        --log bench/results/phase8b_long/progress.log --until-report

只读 requests.jsonl / matrix_report.csv，不碰服务；矩阵被中断时也能从
`last_event_age_s` 看出“停滞”而不是继续空等。
"""
from __future__ import annotations

import argparse
import json
import time
from collections import Counter
from pathlib import Path


def _safe_int(value: object, default: int = 0) -> int:
    """脏字段（null/字符串/缺字段）归默认值，不让监控进程崩在一行坏数据上。"""
    try:
        return int(value)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return default


def snapshot(out_dir: Path) -> dict:
    req_path = out_dir / "requests.jsonl"
    report_path = out_dir / "matrix_report.csv"
    rows = 0
    statuses: Counter[str] = Counter()
    cells: dict[tuple[int, str, int], int] = {}
    last_mtime: float | None = None
    if req_path.exists():
        last_mtime = req_path.stat().st_mtime
        with req_path.open(encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                try:
                    record = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if not isinstance(record, dict):
                    continue
                rows += 1
                statuses[str(record.get("status", "unknown"))] += 1
                key = (_safe_int(record.get("concurrency", 0)), str(record.get("tier", "")),
                       _safe_int(record.get("round", 0)))
                cells[key] = cells.get(key, 0) + 1
    age = None if last_mtime is None else max(time.time() - last_mtime, 0.0)
    return {
        "rows": rows,
        "ok": statuses.get("ok", 0),
        "failed": rows - statuses.get("ok", 0),
        "cells": {f"p{k[0]}/{k[1]}/r{k[2]}": v for k, v in sorted(cells.items())},
        "last_event_age_s": None if age is None else round(age, 1),
        "report": report_path.exists() and (out_dir / "repro_manifest.json").exists(),
    }


def render(state: dict, now: str) -> str:
    age = state["last_event_age_s"]
    age_text = "n/a" if age is None else f"{age:.1f}s"
    cells = " ".join(f"{name}={count}" for name, count in state["cells"].items()) or "no-cell"
    return (
        f"[watch] {now} rows={state['rows']} ok={state['ok']} failed={state['failed']} "
        f"{cells} last_event={age_text} report={'yes' if state['report'] else 'no'}"
    )


def decide(state: dict, stall_after: float, until_report: bool) -> tuple[str, bool]:
    """纯函数：按快照给出 STALL 标注后缀与是否退出；不触碰时间与文件，便于单测。"""
    age = state["last_event_age_s"]
    suffix = ""
    if age is not None and age >= stall_after:
        suffix = f" STALL(>{stall_after:.0f}s)"
    return suffix, (until_report and bool(state["report"]))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="矩阵进度轮询")
    parser.add_argument("--out", required=True, help="矩阵输出目录")
    parser.add_argument("--log", default="", help="进度日志；留空只写 stdout")
    parser.add_argument("--interval", type=float, default=10.0)
    parser.add_argument("--until-report", action="store_true", help="报告出现后退出")
    parser.add_argument("--stall-after", type=float, default=120.0,
                        help="超过该秒数无新请求就标 STALL")
    args = parser.parse_args(argv)

    out_dir = Path(args.out)
    log_path = Path(args.log) if args.log else None
    if log_path:
        log_path.parent.mkdir(parents=True, exist_ok=True)
    while True:
        state = snapshot(out_dir)
        line = render(state, time.strftime("%H:%M:%S"))
        suffix, should_exit = decide(state, args.stall_after, args.until_report)
        line += suffix
        print(line, flush=True)
        if log_path:
            with log_path.open("a", encoding="utf-8") as handle:
                handle.write(line + "\n")
        if should_exit:
            return 0
        time.sleep(args.interval)


if __name__ == "__main__":
    raise SystemExit(main())
