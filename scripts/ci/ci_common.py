"""CI 公共工具：结果模型、字数口径、规则标记扫描、报告输出。

只依赖 Python 标准库，Windows 与 Linux 行为一致。
"""

from __future__ import annotations

import json
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
REPORTS_DIR = REPO_ROOT / "build" / "ci-reports"

# 所有规则的唯一命名空间，检查器用它判断 ID 是否合法。
RULE_ID_RE = re.compile(r"CI-[A-Z]{3}-\d{3}")

# 代码中的规则标记：`# rule: CI-DOC-001` 或 docstring 首行 `rule: CI-DOC-001`。
MARKER_RE = re.compile(r"rule:\s*(CI-[A-Z]{3}-\d{3})")


def guard_console() -> None:
    """保证非 UTF-8 终端（Windows cp936）也能打印中文而不崩。"""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


@dataclass
class Violation:
    rule: str
    path: str
    message: str
    severity: str = "error"   # error | warning
    waived: bool = False


@dataclass
class CheckResult:
    name: str
    violations: list[Violation] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    skipped: bool = False
    skip_reason: str = ""

    @property
    def errors(self) -> list[Violation]:
        return [v for v in self.violations if v.severity == "error" and not v.waived]

    @property
    def passed(self) -> bool:
        return not self.errors


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def count_words(text: str) -> int:
    """字数口径：中文字符逐字计 1，ASCII 词/路径计 1，Markdown 符号不计。"""
    text = re.sub(r"```.*?```", "", text, flags=re.S)
    cjk = len(re.findall(r"[\u4e00-\u9fff]", text))
    words = len(re.findall(r"[A-Za-z0-9_][A-Za-z0-9_./:+@-]*", text))
    return cjk + words


def split_table_row(line: str) -> list[str]:
    """把 Markdown 表格行拆成单元格，去掉首尾竖线与反引号。"""
    cells = [c.strip().strip("`").strip() for c in line.strip().strip("|").split("|")]
    return cells


def iter_table_rows(text: str, header_first_cell: str) -> list[list[str]]:
    """返回表头首列为指定值的表格数据行（跳过表头与分隔行）。"""
    rows: list[list[str]] = []
    in_table = False
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped.startswith("|"):
            in_table = False
            continue
        cells = split_table_row(stripped)
        if not in_table:
            if cells and cells[0] == header_first_cell:
                in_table = True
            continue
        if all(re.fullmatch(r":?-{2,}:?", c) for c in cells if c):
            continue
        if cells and cells[0].startswith(("(", "（")):
            continue
        rows.append(cells)
    return rows


def rule_markers(root: Path, suffixes: tuple[str, ...] = (".py", ".yml", ".yaml")) -> dict[str, list[str]]:
    """扫描代码中的 rule 标记，返回 规则ID -> 文件列表。"""
    found: dict[str, list[str]] = {}
    for path in sorted(root.rglob("*")):
        if path.suffix not in suffixes or not path.is_file():
            continue
        try:
            text = read_text(path)
        except OSError:
            continue
        for match in MARKER_RE.finditer(text):
            found.setdefault(match.group(1), []).append(str(path.relative_to(root)))
    return found


def declared_rules(spec_text: str) -> dict[str, str]:
    """解析规范中的规则表，返回 规则ID -> 实现列。"""
    declared: dict[str, str] = {}
    for line in spec_text.splitlines():
        cells = split_table_row(line) if line.strip().startswith("|") else []
        if len(cells) >= 6 and RULE_ID_RE.fullmatch(cells[0]):
            declared[cells[0]] = cells[4]
    return declared


def emit(result: CheckResult, as_json: bool) -> None:
    if as_json:
        return
    status = "SKIP" if result.skipped else ("PASS" if result.passed else "FAIL")
    print(f"[{status}] {result.name}")
    if result.skip_reason:
        print(f"       skip: {result.skip_reason}")
    for note in result.notes:
        print(f"       note: {note}")
    for v in result.violations:
        if v.waived:
            print(f"       waived: [{v.rule}] {v.path}: {v.message}")
        elif v.severity == "warning":
            print(f"       warn: [{v.rule}] {v.path}: {v.message}")
        else:
            print(f"       error: [{v.rule}] {v.path}: {v.message}")


def write_report(results: list[CheckResult], stem: str = "report") -> Path:
    REPORTS_DIR.mkdir(parents=True, exist_ok=True)
    payload = {
        "passed": all(r.passed for r in results),
        "checks": [
            {
                "name": r.name,
                "passed": r.passed,
                "skipped": r.skipped,
                "skip_reason": r.skip_reason,
                "notes": r.notes,
                "violations": [
                    {
                        "rule": v.rule,
                        "path": v.path,
                        "message": v.message,
                        "severity": v.severity,
                        "waived": v.waived,
                    }
                    for v in r.violations
                ],
            }
            for r in results
        ],
    }
    out = REPORTS_DIR / f"{stem}.json"
    out.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    return out


def write_summary(results: list[CheckResult]) -> None:
    """写 GitHub job summary；本地运行时静默。"""
    path = REPORTS_DIR / "summary.md"
    REPORTS_DIR.mkdir(parents=True, exist_ok=True)
    lines = ["# CI 报告", "", "| 检查 | 结果 | 详情 |", "| --- | --- | --- |"]
    for r in results:
        if r.skipped:
            state, detail = "SKIP", r.skip_reason
        elif r.passed:
            state = "PASS"
            detail = f"{len(r.violations)} 条提示" if r.violations else ""
        else:
            state = "FAIL"
            detail = f"{len(r.errors)} 条违规"
        lines.append(f"| {r.name} | {state} | {detail} |")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    if p := os.environ.get("GITHUB_STEP_SUMMARY"):
        with Path(p).open("a", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")


def skip_reason(rule: str, reason: str) -> CheckResult:
    """rule: CI-GAT-002 —— 跳过必须留痕，原因写入报告，不允许静默通过。"""
    result = CheckResult(name=f"skip:{rule}", skipped=True, skip_reason=reason)
    return result
