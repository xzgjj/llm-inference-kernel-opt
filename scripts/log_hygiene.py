#!/usr/bin/env python3
"""logs/ 与运行证据的保留执行器。默认 dry-run，只报告不删除。

规则见 docs/ci/log-policy.md：
- logs/server/  ：按文件保留最新 N 个（默认 5），总量超限时从最旧的删
- logs/crash/   ：保留最新 2 份
- bench/results/：永不自动删，除非显式 --prune-runs N

用法：
    python scripts/log_hygiene.py                 # 只报告
    python scripts/log_hygiene.py --apply         # 执行 logs/ 清理
    python scripts/log_hygiene.py --prune-runs 20 --apply   # 兼清 run 目录（危险）
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
from pathlib import Path

SERVER_DIR = Path("logs") / "server"
CRASH_DIR = Path("logs") / "crash"
RESULTS_DIR = Path("bench") / "results"

MB = 1024 * 1024


def _size(path: Path) -> int:
    if path.is_file():
        return path.stat().st_size
    return sum(p.stat().st_size for p in path.rglob("*") if p.is_file())


def _human(num: int) -> str:
    if num >= MB:
        return f"{num / MB:.1f} MB"
    return f"{num / 1024:.0f} KB"


def _oldest_first(files: list[Path]) -> list[Path]:
    return sorted(files, key=lambda p: (p.stat().st_mtime, p.name))


def _plan_prune(files: list[Path], keep: int) -> list[Path]:
    ordered = _oldest_first(files)
    return ordered[: max(0, len(ordered) - keep)]


# spdlog rotating_file_sink 产出 server.log（当前）+ server.1.log … server.N.log（数字越大越旧）；
# 兼容 logrotate 风格的 server.log.N。未命中的文件不进清理范围，只报告。
_ROTATE_PATTERNS = (re.compile(r"^server\.log\.(\d+)$"), re.compile(r"^server\.(\d+)\.log$"))


def _rotation_rank(name: str) -> int | None:
    if name == "server.log":
        return -1
    for pattern in _ROTATE_PATTERNS:
        match = pattern.match(name)
        if match:
            return int(match.group(1))
    return None


def _rotation_order(files: list[Path]) -> list[Path]:
    """按"最旧 → 最新"排序；未命中的文件返回为空，不参与清理。"""
    ranked = [(rank, p) for p in files if (rank := _rotation_rank(p.name)) is not None]
    ranked.sort(key=lambda item: -item[0])
    return [path for _, path in ranked]


def _run_dirs(root: Path) -> list[Path]:
    base = root / RESULTS_DIR
    if not base.exists():
        return []
    return sorted(p for p in base.glob("phase*/runs/*") if p.is_dir())


def _remove(path: Path, apply: bool, actions: list[str]) -> None:
    actions.append(f"{'删除' if apply else '将删除'}  {path}")
    if apply:
        if path.is_dir():
            shutil.rmtree(path, ignore_errors=True)
        else:
            path.unlink(missing_ok=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="logs/ 保留策略执行器")
    parser.add_argument("--root", default=None, help="仓库根目录，默认取脚本所在目录的上级")
    parser.add_argument("--keep-server", type=int, default=5, help="logs/server 保留文件数")
    parser.add_argument("--keep-crash", type=int, default=2, help="logs/crash 保留文件数")
    parser.add_argument("--limit-mb", type=int, default=200, help="logs/ 总量上限（MB）")
    parser.add_argument("--prune-runs", type=int, default=0, help="保留最新 N 个 run 目录；0 = 不处理")
    parser.add_argument("--apply", action="store_true", help="真正执行删除；缺省只报告")
    args = parser.parse_args(argv)

    root = Path(args.root).resolve() if args.root else Path(__file__).resolve().parents[1]
    server = root / SERVER_DIR
    crash = root / CRASH_DIR

    for d in (server, crash):
        d.mkdir(parents=True, exist_ok=True)

    actions: list[str] = []
    total_before = _size(server) + _size(crash)

    print(f"[logs] root = {root}")
    print(f"[logs] 当前占用 server={_human(_size(server))} crash={_human(_size(crash))} 合计={_human(total_before)}")

    # 1) logs/server：按轮转命名排序（数字越大越旧），保留最新 N 个
    server_files = [p for p in server.iterdir() if p.is_file()]
    rotated = _rotation_order(server_files)
    unknown = [p.name for p in server_files if _rotation_rank(p.name) is None]
    if unknown:
        print(f"[logs] 非轮转文件不参与清理（{len(unknown)} 个）：{', '.join(sorted(unknown)[:5])}")
    prune = rotated[: max(0, len(rotated) - args.keep_server)]
    for path in prune:
        _remove(path, args.apply, actions)

    # 2) 总量超限：继续删最旧的 server 文件直到回到上限内
    limit = args.limit_mb * MB
    survivors = [p for p in rotated if p not in prune]
    projected = sum(p.stat().st_size for p in survivors) + _size(crash)
    for path in survivors:
        if projected <= limit:
            break
        projected -= path.stat().st_size
        _remove(path, args.apply, actions)

    # 3) logs/crash：按份数
    crash_files = [p for p in crash.iterdir() if p.is_file()]
    for path in _plan_prune(crash_files, args.keep_crash):
        _remove(path, args.apply, actions)

    # 4) bench/results：只统计，除非显式要求
    runs = _run_dirs(root)
    if runs:
        print(f"[runs] {len(runs)} 个 run 目录，合计 {_human(sum(_size(p) for p in runs))}；最新 = {runs[-1].name}")
    else:
        print("[runs] 尚无 run 目录")
    if args.prune_runs > 0 and len(runs) > args.prune_runs:
        for path in runs[: len(runs) - args.prune_runs]:
            _remove(path, args.apply, actions)

    if actions:
        for line in actions:
            print("       " + line)
    else:
        print("[logs] 无需要清理的项")

    after = _size(server) + _size(crash)
    print(f"[logs] {'执行后' if args.apply else '预计'}占用={_human(after)} / 上限 {args.limit_mb} MB")
    if not args.apply and actions:
        print("[提示] 当前为 dry-run；加 --apply 才会真正删除")
    return 0


if __name__ == "__main__":
    sys.exit(main())
