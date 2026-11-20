"""本地入口：用与 CI 相同的判定跑闸门，任何只在 CI 复现的失败都算规则缺陷。

rule: CI-GAT-005
"""

from __future__ import annotations

import argparse
import importlib.util
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_code
import check_diff
import check_docs
import check_tests
from ci_common import CheckResult, emit, guard_console, write_report, write_summary

# 覆盖率脚本按文件路径加载：避免与 site-packages 里的 coverage 包同名冲突。
_spec = importlib.util.spec_from_file_location(
    "ci_coverage_gate", Path(__file__).resolve().parent / "check_coverage.py")
coverage_gate = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(coverage_gate)

MODES = {
    "fast": ["docs", "tests", "code"],
    "pr": ["docs", "tests", "code", "diff"],
    "nightly": ["docs", "tests", "code", "diff", "coverage"],
}


def default_base() -> str:
    """未显式指定基线时：优先当前分支 upstream，回落 origin/main，并打印实际基线。"""
    for cmd in (["git", "rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{u}"],
                ["git", "symbolic-ref", "--short", "refs/remotes/origin/HEAD"]):
        proc = subprocess.run(cmd, cwd=Path(__file__).resolve().parents[2],
                              capture_output=True, text=True)
        if proc.returncode == 0 and proc.stdout.strip():
            return proc.stdout.strip()
    return "origin/main"


def run_step(step: str, base: str | None, labels: str, enforce: bool,
             coverage_build_dir: str = "") -> CheckResult:
    if step == "docs":
        return check_docs.run()
    if step == "tests":
        return check_tests.run()
    if step == "code":
        return check_code.run()
    if step == "coverage":
        return coverage_gate.run(base=base or "", build_dir=coverage_build_dir)
    return check_diff.run(base, labels, enforce)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="CI 本地入口")
    parser.add_argument("--mode", choices=sorted(MODES), default="fast")
    parser.add_argument("--base", default=None)
    parser.add_argument("--labels", default="")
    parser.add_argument("--enforce", action="store_true", help="强制执行标签规则（PR 环境）")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--coverage-build-dir", default="",
                        help="带 gcov 插桩的构建目录；默认自动探测 build-cov → build")
    args = parser.parse_args(argv)

    guard_console()
    if args.list:
        for mode, steps in MODES.items():
            print(f"{mode}: {' -> '.join(steps)}")
        return 0

    if args.base is None:
        args.base = default_base()
        print(f"base: {args.base}（可用 --base 覆盖）")

    results = []
    for step in MODES[args.mode]:
        print(f"\n== {step} ==")
        result = run_step(step, args.base, args.labels, args.enforce, args.coverage_build_dir)
        emit(result, as_json=False)
        results.append(result)

    print("\n== summary ==")
    failed = [r for r in results if not r.passed]
    for result in results:
        state = "SKIP" if result.skipped else ("PASS" if result.passed else "FAIL")
        print(f"  {state:4} {result.name}")
    write_report(results, stem="ci-report")
    write_summary(results)
    if failed:
        print(f"FAIL: {len(failed)} 个闸门未通过")
        return 1
    print("PASS: 全部闸门通过")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
