"""测试注册表闸门：把 docs/testing.md 当作测试策略的唯一事实来源。

rule: CI-TST-001
"""

from __future__ import annotations

import argparse
import re
from datetime import date, timedelta
from pathlib import Path

from ci_common import (
    REPO_ROOT,
    CheckResult,
    Violation,
    emit,
    guard_console,
    iter_table_rows,
    read_text,
    write_report,
)

REGISTRY = "docs/testing.md"
REQUIRED_HEADER = "| Scope | Level | Runner | Args | Metadata |"

LEVELS = {"unit", "contract", "integration", "e2e", "perf", "sanitizer"}
RUNNERS = {"python", "ctest", "cmake", "bash", "nvcc"}
SPEED_TAGS = {"fast", "slow"}
ENV_TAGS = {"hermetic", "needs-network", "needs-gpu", "needs-vm"}
EXCLUDED_DIRS = {"fixtures", "data", "__pycache__", "golden"}
# 产品测试与 CI 工具自测分两棵目录树，注册表同一份，扫描两处保证都登记。
TEST_ROOTS = ("tests", "ci")


def _rows() -> list[list[str]]:
    text = read_text(REPO_ROOT / REGISTRY)
    return iter_table_rows(text, "Scope")


def _test_files() -> list[str]:
    found = []
    for root_name in TEST_ROOTS:
        root = REPO_ROOT / root_name
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix not in {".py", ".cc", ".cpp", ".cxx", ".h", ".hpp"}:
                continue
            if any(part in EXCLUDED_DIRS for part in path.parts):
                continue
            name = path.name
            if name.endswith(("_test.py", "_test.cc", "_test.cpp")) or name.startswith("test_"):
                found.append(str(path.relative_to(REPO_ROOT)).replace("\\", "/"))
    return sorted(found)


def _tags(cell: str) -> list[str]:
    return [t.strip().strip("`").strip() for t in re.split(r",|，", cell) if t.strip()]


def registry_table(result: CheckResult) -> None:
    """rule: CI-TST-001 —— 五列齐全、Level 与 Metadata 取值合法、Runner 在词表内。"""
    text = read_text(REPO_ROOT / REGISTRY) if (REPO_ROOT / REGISTRY).exists() else ""
    if REQUIRED_HEADER not in text:
        result.violations.append(Violation("CI-TST-001", REGISTRY, f"缺少注册表表头 {REQUIRED_HEADER}"))
        return
    scopes: dict[str, int] = {}
    for row in _rows():
        if len(row) != 5:
            result.violations.append(Violation("CI-TST-001", REGISTRY, f"行列数 {len(row)} != 5：{row}"))
            continue
        scope, level, runner, args, metadata = row
        if level not in LEVELS:
            result.violations.append(Violation("CI-TST-001", REGISTRY, f"{scope}: Level 非法 {level}"))
        if runner not in RUNNERS:
            result.violations.append(Violation("CI-TST-001", REGISTRY, f"{scope}: Runner 非法 {runner}"))
        if not args:
            result.violations.append(Violation("CI-TST-001", REGISTRY, f"{scope}: Args 为空"))
        tags = _tags(metadata)
        if not any(t in SPEED_TAGS for t in tags):
            result.violations.append(Violation("CI-TST-001", REGISTRY, f"{scope}: 缺少 fast/slow 标签"))
        if not any(t in ENV_TAGS for t in tags):
            result.violations.append(Violation("CI-TST-001", REGISTRY, f"{scope}: 缺少环境标签"))
        scopes[scope] = scopes.get(scope, 0) + 1
    for scope, count in scopes.items():
        if count > 1:
            result.violations.append(
                Violation("CI-TST-001", REGISTRY, f"Scope 重复登记 {count} 次：{scope}")
            )


def not_empty(result: CheckResult) -> None:
    """rule: CI-TST-002 —— 有测试文件就必须有登记；空仓库允许空表。"""
    files = _test_files()
    rows = _rows()
    result.notes.append(f"测试文件 {len(files)} 个，登记 {len(rows)} 条")
    if files and not rows:
        result.violations.append(
            Violation("CI-TST-002", REGISTRY, f"存在 {len(files)} 个测试文件但注册表为空")
        )


def unregistered(result: CheckResult) -> None:
    """rule: CI-TST-003 —— tests/** 下每个测试文件必须被某条 Scope 覆盖。"""
    scopes = [row[0] for row in _rows() if row]
    for rel in _test_files():
        covered = False
        for scope in scopes:
            scope_rel = scope.strip().rstrip("/")
            if not scope_rel:
                continue
            if rel == scope_rel or rel.startswith(scope_rel + "/"):
                covered = True
                break
        if not covered:
            result.violations.append(
                Violation("CI-TST-003", rel, "未被任何 Scope 覆盖 → 在 " + REGISTRY + " 增加一行")
            )


def stale_entry(result: CheckResult) -> None:
    """rule: CI-TST-004 —— Scope 目录不存在或内部无测试，视为过期登记。"""
    files = _test_files()
    for row in _rows():
        if not row:
            continue
        scope = row[0]
        path = REPO_ROOT / scope
        if not path.exists():
            result.violations.append(Violation("CI-TST-004", REGISTRY, f"Scope 目录不存在：{scope}"))
            continue
        inside = [f for f in files if f == scope or f.startswith(scope.rstrip("/") + "/")]
        if not inside:
            result.violations.append(Violation("CI-TST-004", REGISTRY, f"Scope 内没有任何测试：{scope}"))


def env_partition(result: CheckResult) -> None:
    """rule: CI-TST-005 —— 需要 GPU/虚拟机/网络的测试不得标成 fast 进 PR 闸门。"""
    for row in _rows():
        if len(row) != 5:
            continue
        scope, _, _, _, metadata = row
        tags = _tags(metadata)
        heavy = {"needs-gpu", "needs-vm", "needs-network"} & set(tags)
        if heavy and "fast" in tags:
            result.violations.append(
                Violation("CI-TST-005", REGISTRY, f"{scope}: 同时标了 {sorted(heavy)} 与 fast")
            )


def quarantine_ttl(result: CheckResult) -> None:
    """rule: CI-TST-007 —— 隔离期 ≤14 天，必须带到期日与 owner。"""
    today = date.today()
    for row in _rows():
        if len(row) < 5:
            continue
        scope, metadata = row[0], row[4]
        for tag in _tags(metadata):
            if not tag.startswith("quarantine:"):
                continue
            parts = tag.split(":")
            if len(parts) != 3 or not parts[2].strip():
                result.violations.append(
                    Violation("CI-TST-007", REGISTRY, f"{scope}: 隔离标签格式应为 quarantine:YYYY-MM-DD:owner")
                )
                continue
            try:
                expires = date.fromisoformat(parts[1])
            except ValueError:
                result.violations.append(Violation("CI-TST-007", REGISTRY, f"{scope}: 到期日非法 {parts[1]}"))
                continue
            if expires < today:
                result.violations.append(Violation("CI-TST-007", REGISTRY, f"{scope}: 隔离已过期 {expires}"))
            elif expires > today + timedelta(days=14):
                result.violations.append(
                    Violation("CI-TST-007", REGISTRY, f"{scope}: 隔离期超过 14 天（{expires}）")
                )


CHECKS = [registry_table, not_empty, unregistered, stale_entry, env_partition, quarantine_ttl]


def run() -> CheckResult:
    result = CheckResult("tests")
    for check in CHECKS:
        check(result)
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="测试注册表闸门")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    guard_console()
    result = run()
    emit(result, args.json)
    write_report([result], stem="report-tests")
    return 0 if result.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
