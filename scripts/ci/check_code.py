"""代码质量快检：格式、复杂度、重复率、无主 TODO。

没有源码时整体跳过并说明原因，不伪装成通过。

rule: CI-COD-001
"""

from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

from ci_common import REPO_ROOT, CheckResult, Violation, emit, guard_console, read_text, write_report

SOURCE_SUFFIXES = {".cc", ".cpp", ".cxx", ".h", ".hpp", ".cu", ".cuh"}
SOURCE_DIRS = ("src", "include", "benchmarks")
COMPLEXITY_LIMIT = 25
FUNCTION_LINES_LIMIT = 60
DUP_WINDOW = 6
DUP_WARN = 0.03
DUP_FAIL = 0.05
TODO_RE = re.compile(r"(TODO|FIXME|XXX)(?!\()")
FUNC_RE = re.compile(r"^[A-Za-z_][\w:<>,*&\s]*?\b(\w+)\s*\([^;]*\)\s*(const)?\s*\{")
DECISION_RE = re.compile(r"\b(if|for|while|case|catch)\b|&&|\|\||\?")


def source_files() -> list[Path]:
    files: list[Path] = []
    for name in SOURCE_DIRS:
        root = REPO_ROOT / name
        if root.exists():
            files.extend(p for p in root.rglob("*") if p.is_file() and p.suffix in SOURCE_SUFFIXES)
    return sorted(files)


def _no_source(result: CheckResult) -> bool:
    if source_files():
        return False
    result.skipped = True
    result.skip_reason = "src/ 与 include/ 下尚无 C++ 源码，跳过代码质量检查"
    return True


def format_check(result: CheckResult) -> None:
    """rule: CI-COD-001 —— clang-format 零差异；工具缺失时明确失败而非静默。"""
    if _no_source(result):
        return
    tool = shutil.which("clang-format")
    if not tool:
        result.violations.append(
            Violation("CI-COD-001", "toolchain", "clang-format 不在 PATH；CI 镜像必须预装")
        )
        return
    bad = []
    for path in source_files():
        proc = subprocess.run(
            [tool, "--dry-run", "-Werror", str(path)],
            cwd=REPO_ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        if proc.returncode != 0:
            bad.append(str(path.relative_to(REPO_ROOT)).replace("\\", "/"))
    for path in bad:
        result.violations.append(Violation("CI-COD-001", path, "格式不符合 .clang-format，运行 clang-format -i"))


def complexity(result: CheckResult) -> None:
    """rule: CI-COD-003 —— 认知复杂度 ≤25、函数体 ≤60 行；超限需拆分。"""
    if _no_source(result):
        return
    for path in source_files():
        lines = read_text(path).splitlines()
        index = 0
        while index < len(lines):
            match = FUNC_RE.match(lines[index])
            if not match:
                index += 1
                continue
            depth, end, decisions, opened = 0, index, 0, False
            for cursor in range(index, len(lines)):
                text = lines[cursor]
                depth += text.count("{") - text.count("}")
                opened = opened or "{" in text
                if cursor > index:
                    decisions += len(DECISION_RE.findall(text))
                if opened and depth <= 0:
                    end = cursor
                    break
            length = end - index + 1
            if decisions > COMPLEXITY_LIMIT or length > FUNCTION_LINES_LIMIT:
                rel = str(path.relative_to(REPO_ROOT)).replace("\\", "/")
                result.violations.append(
                    Violation(
                        "CI-COD-003",
                        f"{rel}:{index + 1}",
                        f"{match.group(1)} 复杂度 {decisions}/{COMPLEXITY_LIMIT}、行数 {length}/{FUNCTION_LINES_LIMIT}",
                    )
                )
            index = end + 1


def _normalize(path: Path) -> list[str]:
    out = []
    in_block = False
    for line in read_text(path).splitlines():
        stripped = line.strip()
        if stripped.startswith("/*"):
            in_block = True
        if in_block:
            if "*/" in stripped:
                in_block = False
            continue
        stripped = re.sub(r"//.*$", "", stripped)
        if stripped:
            out.append(re.sub(r"\s+", " ", stripped))
    return out


def duplication(result: CheckResult) -> None:
    """rule: CI-COD-004 —— 6 行窗口的重复代码率 >3% 警告、>5% 失败。"""
    if _no_source(result):
        return
    windows: dict[str, list[str]] = {}
    total = 0
    for path in source_files():
        lines = _normalize(path)
        total += len(lines)
        for start in range(0, max(0, len(lines) - DUP_WINDOW + 1)):
            chunk = "\n".join(lines[start:start + DUP_WINDOW])
            digest = hashlib.sha1(chunk.encode("utf-8")).hexdigest()
            windows.setdefault(digest, []).append(
                f"{path.relative_to(REPO_ROOT).as_posix()}:{start + 1}"
            )
    dup_hashes = [digest for digest, entries in windows.items() if len(entries) > 1]
    duplicated = DUP_WINDOW * len(dup_hashes)
    ratio = (duplicated / total) if total else 0.0
    result.notes.append(f"重复代码率 {ratio:.2%}（阈值 {DUP_WARN:.0%} 警告 / {DUP_FAIL:.0%} 失败）")
    if ratio > DUP_FAIL:
        result.violations.append(Violation("CI-COD-004", "src", f"重复率 {ratio:.2%} > {DUP_FAIL:.0%}"))
    elif ratio > DUP_WARN:
        result.violations.append(
            Violation("CI-COD-004", "src", f"重复率 {ratio:.2%} > {DUP_WARN:.0%}（警告）", severity="warning")
        )


def todos(result: CheckResult) -> None:
    """rule: CI-COD-006 —— 只允许 TODO(owner): 形式，无主 TODO 会永久留在代码里。"""
    if _no_source(result):
        return
    for path in source_files():
        for number, line in enumerate(read_text(path).splitlines(), start=1):
            if TODO_RE.search(line) and "TODO(" not in line:
                result.violations.append(
                    Violation(
                        "CI-COD-006",
                        f"{path.relative_to(REPO_ROOT).as_posix()}:{number}",
                        "TODO/FIXME 缺少责任人，改为 TODO(owner): 说明",
                    )
                )


def phase4_scanners(result: CheckResult) -> None:
    """rule: CI-P4-001 —— 4-G4/4-G5：依赖、规模、范围三条扫描随代码闸门跑。"""
    if _no_source(result):
        return
    scanners = (
        ("CI-P4-001", "check_deps.py", "依赖清单与实际链接不一致"),
        ("CI-P4-002", "size_dep_check.py", "规模/模块/依赖超限"),
        ("CI-P4-003", "scope_scan.py", "非目标形态或测试未登记"),
    )
    for code, script, label in scanners:
        proc = subprocess.run(
            [sys.executable, str(REPO_ROOT / "scripts" / "ci" / script)],
            cwd=REPO_ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        if proc.returncode != 0:
            detail = (proc.stdout + proc.stderr).strip().splitlines()
            result.violations.append(
                Violation(code, f"scripts/ci/{script}", f"{label}: {detail[-1] if detail else 'rc!=0'}")
            )
        else:
            result.notes.append(proc.stdout.strip())


CHECKS = [format_check, complexity, duplication, todos, phase4_scanners]


def run() -> CheckResult:
    result = CheckResult("code")
    for check in CHECKS:
        check(result)
        if result.skipped:
            break
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="代码质量快检")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    guard_console()
    result = run()
    emit(result, args.json)
    write_report([result], stem="report-code")
    return 0 if result.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
