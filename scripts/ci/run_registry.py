"""按 docs/testing.md 的注册表执行测试：表是唯一事实来源，命令不在 workflow 里重复。

rule: CI-TST-005

--build-dir 用于把表里固定的 `--test-dir build` 改指向当前作业的构建目录
（sanitizer 作业建 build-san-*，不改表也能跑到带插桩的二进制）。
--junit-dir 让 ctest 额外产出一份 JUnit XML，供 CI 归档。
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

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


def apply_build_dir(args: list[str], build_dir: str) -> list[str]:
    """把 `--test-dir build` 换成本次作业的构建目录，其余参数原样保留。"""
    if not build_dir:
        return args
    out: list[str] = []
    i = 0
    while i < len(args):
        if args[i] == "--test-dir" and i + 1 < len(args) and args[i + 1] == "build":
            out.extend(["--test-dir", build_dir])
            i += 2
            continue
        if args[i] == "--test-dir=build":
            out.append(f"--test-dir={build_dir}")
        else:
            out.append(args[i])
        i += 1
    return out


def _scope_slug(scope: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", scope.strip()) or "scope"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="按注册表执行测试")
    parser.add_argument("--speed", choices=["fast", "slow", "all"], default="fast")
    parser.add_argument("--exclude", default="needs-gpu,needs-vm,needs-network")
    parser.add_argument("--only", default="")
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--build-dir", default="", help="覆盖注册表里的 --test-dir build")
    parser.add_argument("--junit-dir", default="", help="ctest 结果另存 JUnit XML 的目录")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    guard_console()
    result = CheckResult("registry-run")
    exclude = [tag.strip() for tag in args.exclude.split(",") if tag.strip()]
    rows = select(load_rows(), args.speed, exclude, args.only)
    if not rows:
        # 空跑不是通过：nightly 没有 slow 登记时必须显式说明为什么什么都没跑。
        note = f"没有匹配的登记（speed={args.speed}, exclude={exclude}）→ SKIP，不计为通过"
        print(f"SKIP  {note}", flush=True)
        result.notes.append(note)
        emit(result, args.json)
        write_report([result], stem="report-registry")
        return 0

    for row in rows:
        runner_path = resolve_runner(row["runner"])
        if not runner_path:
            result.violations.append(
                Violation("CI-TST-005", row["scope"], f"Runner {row['runner']} 不可执行")
            )
            continue
        cmd = [runner_path, *shlex.split(row["args"], posix=(os.name != "nt"))]
        cmd = apply_build_dir(cmd, args.build_dir)
        if args.junit_dir and row["runner"] == "ctest":
            junit_path = Path(args.junit_dir) / f"{_scope_slug(row['scope'])}.xml"
            junit_path.parent.mkdir(parents=True, exist_ok=True)
            cmd.extend(["--output-junit", str(junit_path)])
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
