"""按 docs/testing.md 的注册表执行测试：表是唯一事实来源，命令不在 workflow 里重复。

rule: CI-TST-005
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys

from ci_common import REPO_ROOT, CheckResult, Violation, emit, guard_console, iter_table_rows, read_text, write_report

REGISTRY = "docs/testing.md"
RUNNER_MAP = {"python": sys.executable}


def load_rows() -> list[dict]:
    text = read_text(REPO_ROOT / REGISTRY)
    rows = []
    for cells in iter_table_rows(text, "Scope"):
        if len(cells) != 5:
            continue
        scope, level, runner, args, metadata = cells
        tags = [t.strip().strip("`").strip() for t in re.split(r",|，", metadata) if t.strip()]
        rows.append({"scope": scope, "level": level, "runner": runner, "args": args, "tags": tags})
    return rows


def select(rows: list[dict], speed: str, exclude: list[str], only: str) -> list[dict]:
    chosen = []
    for row in rows:
        if speed != "all" and speed not in row["tags"]:
            continue
        if set(exclude) & set(row["tags"]):
            continue
        if only and only not in row["scope"]:
            continue
        chosen.append(row)
    return chosen


def resolve_runner(name: str) -> str | None:
    return RUNNER_MAP.get(name) or shutil.which(name)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="按注册表执行测试")
    parser.add_argument("--speed", choices=["fast", "slow", "all"], default="fast")
    parser.add_argument("--exclude", default="needs-gpu,needs-vm,needs-network")
    parser.add_argument("--only", default="")
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    guard_console()
    result = CheckResult("registry-run")
    exclude = [tag.strip() for tag in args.exclude.split(",") if tag.strip()]
    rows = select(load_rows(), args.speed, exclude, args.only)
    if not rows:
        result.notes.append(f"没有匹配的登记（speed={args.speed}, exclude={exclude}）")
        emit(result, args.json)
        return 0

    for row in rows:
        runner_path = resolve_runner(row["runner"])
        if not runner_path:
            result.violations.append(
                Violation("CI-TST-005", row["scope"], f"Runner {row['runner']} 不可执行")
            )
            continue
        cmd = [runner_path, *shlex.split(row["args"], posix=(os.name != "nt"))]
        if args.dry_run:
            print(f"DRY  {row['scope']}: {' '.join(cmd)}")
            continue
        print(f"RUN  {row['scope']} ({row['level']}): {' '.join(cmd)}", flush=True)
        try:
            proc = subprocess.run(
                cmd, cwd=REPO_ROOT, capture_output=True, text=True,
                encoding="utf-8", errors="replace", timeout=args.timeout,
            )
        except subprocess.TimeoutExpired:
            result.violations.append(
                Violation("CI-TST-005", row["scope"], f"超过 {args.timeout}s 未结束")
            )
            continue
        output = (proc.stdout or "") + (proc.stderr or "")
        if proc.returncode != 0:
            tail = "\n".join(output.strip().splitlines()[-30:])
            print(tail)
            result.violations.append(
                Violation("CI-TST-005", row["scope"], f"退出码 {proc.returncode}，见上方输出")
            )
        else:
            print(f"OK   {row['scope']}", flush=True)

    result.notes.append(f"执行 {len(rows)} 条登记")
    emit(result, args.json)
    write_report([result], stem="report-registry")
    return 0 if result.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
