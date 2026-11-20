"""本地入口：用与 CI 相同的判定跑闸门，任何只在 CI 复现的失败都算规则缺陷。

rule: CI-GAT-005
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_code
import check_diff
import check_docs
import check_tests
from ci_common import CheckResult, emit, guard_console, write_report, write_summary

MODES = {
    "fast": ["docs", "tests", "code"],
    "pr": ["docs", "tests", "code", "diff"],
    "nightly": ["docs", "tests", "code", "diff"],
}


def run_step(step: str, base: str | None, labels: str, enforce: bool) -> CheckResult:
    if step == "docs":
        return check_docs.run()
    if step == "tests":
        return check_tests.run()
    if step == "code":
        return check_code.run()
    return check_diff.run(base, labels, enforce)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="CI 本地入口")
    parser.add_argument("--mode", choices=sorted(MODES), default="fast")
    parser.add_argument("--base", default=None)
    parser.add_argument("--labels", default="")
    parser.add_argument("--enforce", action="store_true", help="强制执行标签规则（PR 环境）")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args(argv)

    guard_console()
    if args.list:
        for mode, steps in MODES.items():
            print(f"{mode}: {' -> '.join(steps)}")
        return 0

    results = []
    for step in MODES[args.mode]:
        print(f"\n== {step} ==")
        result = run_step(step, args.base, args.labels, args.enforce)
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
