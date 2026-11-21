"""文档闸门：字数预算、规范体积、重复段落、必需要素、链接、规则-实现一致性。

rule: CI-DOC-001
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from ci_common import (
    REPO_ROOT,
    CheckResult,
    Violation,
    count_words,
    declared_rules,
    emit,
    guard_console,
    read_text,
    rule_markers,
    write_report,
)

# 要求类文档：每次迭代都会被完整读取，因此有硬字数上限。
BUDGET_DOCS = {
    "docs/ci.md": 3000,
    "CLAUDE.md": 3000,
    "implementation_plan.md": 3000,
    "AGENT_EXECUTION_PROTOCOL.md": 3000,
    "SPEC.md": 3000,
}

SPEC_DOC = "docs/ci/ci-spec.md"
SPEC_BYTES = (18000, 26000)

DUP_DOCS = [
    "docs/ci.md",
    "docs/ci/ci-spec.md",
    "docs/testing.md",
    "README.md",
]

REQUIRED_SECTIONS = {
    "docs/ci.md": ["分层闸门", "例外", "红线", "run_ci.py"],
    SPEC_DOC: ["规则目录", "例外与批准", "阈值", "人工审查清单", "失败、重试"],
    "docs/testing.md": ["| Scope | Level | Runner | Args | Metadata |", "一致性检查", "待补齐"],
}

LINK_RE = re.compile(r"\[[^\]]*\]\(([^)\s]+)")
NORMALIZE_RE = re.compile(r"[\s`*#>|\-—:：,，。.、;；!！?？()（）\[\]{}\"'“”‘’]+")


def doc_budget(result: CheckResult) -> None:
    """rule: CI-DOC-001 —— 要求类文档 ≤3000 字，超出即失败。"""
    for rel, limit in BUDGET_DOCS.items():
        path = REPO_ROOT / rel
        if not path.exists():
            result.notes.append(f"{rel} 不存在，未纳入（入库后自动生效）")
            continue
        words = count_words(read_text(path))
        result.notes.append(f"{rel}: {words}/{limit} 字")
        if words > limit:
            result.violations.append(
                Violation(
                    "CI-DOC-001",
                    rel,
                    f"字数 {words} > {limit}，删掉背景复述，细则移入 docs/ci/ci-spec.md",
                )
            )


def spec_size(result: CheckResult) -> None:
    """rule: CI-DOC-002 —— 规范细则 18–26 KB：太薄不完整，太厚没人按需查阅。"""
    path = REPO_ROOT / SPEC_DOC
    if not path.exists():
        result.violations.append(Violation("CI-DOC-002", SPEC_DOC, "规范文件缺失"))
        return
    size = path.stat().st_size
    result.notes.append(f"{SPEC_DOC}: {size} bytes")
    low, high = SPEC_BYTES
    if not low <= size <= high:
        hint = "补充判定与豁免细节" if size < low else "删掉重复解释，把流程移入附录"
        result.violations.append(
            Violation("CI-DOC-002", SPEC_DOC, f"体积 {size} bytes 不在 {low}-{high} 区间，{hint}")
        )


def _paragraphs(text: str) -> list[str]:
    blocks, current, in_code = [], [], False
    for line in text.splitlines():
        if line.strip().startswith("```"):
            in_code = not in_code
            continue
        if in_code:
            continue
        if line.strip():
            current.append(line.strip())
        elif current:
            blocks.append("\n".join(current))
            current = []
    if current:
        blocks.append("\n".join(current))
    return blocks


def duplicate_paragraphs(result: CheckResult) -> None:
    """rule: CI-DOC-003 —— 归一化后 ≥40 字的段落不得在文档集中重复出现。"""
    seen: dict[str, list[str]] = {}
    for rel in DUP_DOCS:
        path = REPO_ROOT / rel
        if not path.exists():
            continue
        for index, block in enumerate(_paragraphs(read_text(path)), start=1):
            key = NORMALIZE_RE.sub("", block)
            if len(key) < 40:
                continue
            seen.setdefault(key, []).append(f"{rel}#p{index}")
    for locations in seen.values():
        if len(locations) > 1:
            result.violations.append(
                Violation(
                    "CI-DOC-003",
                    locations[0].split("#")[0],
                    f"同一段落出现在 {', '.join(locations)}；保留一份，其余改为引用",
                )
            )


def required_sections(result: CheckResult) -> None:
    """rule: CI-DOC-004 —— 每份文档必须包含其规定小节，缺一节即失败。"""
    for rel, tokens in REQUIRED_SECTIONS.items():
        path = REPO_ROOT / rel
        if not path.exists():
            continue
        text = read_text(path)
        for token in tokens:
            if token not in text:
                result.violations.append(
                    Violation("CI-DOC-004", rel, f"缺少必需要素：{token}")
                )


def link_integrity(result: CheckResult) -> None:
    """rule: CI-DOC-005 —— 相对链接必须指向真实文件，避免文档互相指向空气。"""
    for rel in DUP_DOCS:
        path = REPO_ROOT / rel
        if not path.exists():
            continue
        for target in LINK_RE.findall(read_text(path)):
            if target.startswith(("http://", "https://", "mailto:", "#")):
                continue
            clean = target.split("#", 1)[0]
            if not clean:
                continue
            resolved = (path.parent / clean).resolve()
            if not resolved.exists():
                result.violations.append(
                    Violation("CI-DOC-005", rel, f"链接目标不存在：{target}")
                )


def rule_implementation_map(result: CheckResult) -> None:
    """rule: CI-DOC-006 —— 文档声明的规则与代码里的 rule 标记必须完全对应。"""
    spec_path = REPO_ROOT / SPEC_DOC
    if not spec_path.exists():
        return
    declared = declared_rules(read_text(spec_path))
    script_markers = rule_markers(REPO_ROOT / "scripts" / "ci")
    workflow_markers = rule_markers(REPO_ROOT / ".github" / "workflows")

    for rule_id, impl in declared.items():
        if impl.startswith("script:"):
            parts = impl[len("script:"):].split(":")
            if len(parts) != 2:
                result.violations.append(
                    Violation("CI-DOC-006", SPEC_DOC, f"{rule_id} 实现格式应为 script:文件:函数")
                )
                continue
            file_name, symbol = parts
            script = REPO_ROOT / "scripts" / "ci" / file_name
            if not script.exists():
                result.violations.append(
                    Violation("CI-DOC-006", SPEC_DOC, f"{rule_id} 指向不存在的脚本 {file_name}")
                )
            elif f"def {symbol}" not in read_text(script):
                result.violations.append(
                    Violation("CI-DOC-006", f"scripts/ci/{file_name}", f"{rule_id} 声明的函数 {symbol} 不存在")
                )
            if rule_id not in script_markers:
                result.violations.append(
                    Violation("CI-DOC-006", SPEC_DOC, f"{rule_id} 在代码中缺少 rule 标记")
                )
        elif impl.startswith("workflow:"):
            job = impl[len("workflow:"):]
            workflow = REPO_ROOT / ".github" / "workflows" / "ci.yml"
            text = read_text(workflow) if workflow.exists() else ""
            if not re.search(rf"^  {re.escape(job)}:", text, re.M):
                result.violations.append(
                    Violation("CI-DOC-006", SPEC_DOC, f"{rule_id} 指向的 job {job} 不在 ci.yml 中")
                )
            if rule_id not in workflow_markers:
                result.violations.append(
                    Violation("CI-DOC-006", SPEC_DOC, f"{rule_id} 在 workflow 中缺少 rule 标记")
                )
        elif impl.startswith("manual:"):
            continue
        else:
            result.violations.append(
                Violation("CI-DOC-006", SPEC_DOC, f"{rule_id} 实现列无法识别：{impl}")
            )

    for rule_id, files in script_markers.items():
        if rule_id not in declared:
            result.violations.append(
                Violation(
                    "CI-DOC-006",
                    files[0],
                    f"{rule_id} 已实现但未写入规则目录（文档滞后于代码）",
                )
            )
    for rule_id, files in workflow_markers.items():
        if rule_id not in declared:
            result.violations.append(
                Violation(
                    "CI-DOC-006",
                    files[0],
                    f"{rule_id} 已写入 workflow 但未登记到规则目录",
                )
            )


def test_counts(result: CheckResult) -> None:
    """rule: CI-DOC-008 —— 文档计数块必须与 CTest 注册表一致，禁止手写数字。"""
    script = REPO_ROOT / "scripts/ci/gen_test_counts.py"
    proc = subprocess.run([sys.executable, str(script), "--check"],
                          capture_output=True, text=True, cwd=REPO_ROOT)
    if proc.returncode != 0:
        detail = (proc.stdout or proc.stderr).strip().replace("\n", " ")
        result.violations.append(
            Violation("CI-DOC-008", "README.md", f"测试计数漂移：{detail}"))
    else:
        result.notes.append((proc.stdout or "").strip().splitlines()[-1])


CHECKS = [
    doc_budget,
    spec_size,
    duplicate_paragraphs,
    required_sections,
    link_integrity,
    rule_implementation_map,
    test_counts,
]


def run(only: str = "") -> CheckResult:
    result = CheckResult("docs")
    for check in CHECKS:
        if only and only not in check.__name__:
            continue
        check(result)
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="文档闸门")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--only", default="", help="只跑名称匹配的检查")
    args = parser.parse_args(argv)

    guard_console()
    result = run(args.only)
    emit(result, args.json)
    write_report([result], stem=f"report-{result.name}")
    return 0 if result.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
