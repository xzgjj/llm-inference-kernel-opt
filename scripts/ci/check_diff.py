"""改动闸门：范围白名单、Diff 预算、规则文件批准、受保护测试、敏感文件、例外校验。

rule: CI-DIF-001
"""

from __future__ import annotations

import argparse
import codecs
import fnmatch
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from datetime import date, timedelta
from pathlib import Path

from ci_common import (
    REPO_ROOT,
    CheckResult,
    Violation,
    declared_rules,
    emit,
    guard_console,
    iter_table_rows,
    read_text,
    write_report,
)

WAIVER_FILE = "docs/ci/waivers.json"
SPEC_FILE = "docs/ci/ci-spec.md"
CODEOWNERS = ".github/CODEOWNERS"

ALLOWED_TOP = {
    "src", "include", "tests", "ci", "docs", "scripts", "cmake", ".github", ".githooks",
    "third_party", "benchmarks", "examples", "tools", "bench", "evidence",
}
ALLOWED_ROOT_FILES = {
    "README.md", "CLI.md", "CMakeLists.txt", "CMakePresets.json", ".gitignore", ".gitattributes",
    "LICENSE", "NOTICE", ".clang-format", ".clang-tidy", ".editorconfig", "CODE_OF_CONDUCT.md",
}
RULE_PATTERNS = [
    ".github/**", ".githooks/**", "scripts/ci/**", "ci/**", "docs/ci/**", "docs/ci.md", "docs/testing.md",
]
CORE_PREFIXES = ("src/", "include/", "cmake/", "CMakeLists.txt", "CMakePresets.json")
GENERATED_PATTERNS = [
    "build/**", "build-*/**", "out/**", "third_party/**", "**/*.o", "**/*.obj", "**/*.so",
    "**/*.a", "**/*.exe", "**/*.dll", "**/*.pyc", "**/__pycache__/**", ".venv/**",
]
SENSITIVE_PATTERNS = [
    "**/*.pem", "**/*.key", "**/*.p12", "**/id_rsa*", "**/.env", "**/.env.*",
    "**/credentials*", "**/secrets*", "**/*.pfx",
]
SKIP_MARKERS = re.compile(r"(?i)(unittest\.skip|DISABLED_|xfail|#if 0|pytest\.mark\.skip|disabled)")
ASSERT_MARKERS = re.compile(r"(?i)(assert|EXPECT_|ASSERT_|require\()")


@dataclass
class Diff:
    files: dict[str, str] = field(default_factory=dict)
    lines: dict[str, tuple[int, int]] = field(default_factory=dict)
    range: str = ""
    available: bool = True
    reason: str = ""

    def text_of(self, path: str, unified: int = 0) -> str:
        if not self.available:
            return ""
        return _git(["diff", f"--unified={unified}", self.range, "--", path])


def _git(args: list[str]) -> str:
    """core.quotepath=false：中文路径不被 git 转义成八进制，否则白名单会误判顶层目录。"""
    proc = subprocess.run(
        ["git", "-c", "core.quotepath=false", *args], cwd=REPO_ROOT, capture_output=True, text=True,
        encoding="utf-8", errors="replace"
    )
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or f"git {' '.join(args)} failed")
    return proc.stdout


def _decode_git_path(raw: str) -> str:
    """兜底解码 git 仍加引号的 C 风格路径（\\ooo 为 UTF-8 字节）。"""
    if len(raw) >= 2 and raw.startswith('"') and raw.endswith('"'):
        body = raw[1:-1]
        return codecs.escape_decode(body.encode("utf-8"))[0].decode("utf-8", "replace")
    return raw


def collect_diff(base: str | None) -> Diff:
    diff = Diff()
    local = not base and not os.environ.get("GITHUB_BASE_REF")
    if base:
        diff.range = f"{base}..."
    elif os.environ.get("GITHUB_BASE_REF"):
        diff.range = f"origin/{os.environ['GITHUB_BASE_REF']}..."
    else:
        # 本地模式：与 origin/main 的两点比较会包含未提交改动，供 pre-commit 使用
        try:
            _git(["rev-parse", "--verify", "origin/main"])
            diff.range = "origin/main"
        except RuntimeError:
            try:
                _git(["rev-parse", "--verify", "HEAD"])
                diff.range = "HEAD"
                diff.reason = "无 origin/main，比较工作区改动"
            except RuntimeError:
                diff.available = False
                diff.reason = "仓库尚无提交，跳过改动检查"
                return diff

    try:
        for line in _git(["diff", "--name-status", "--no-renames", diff.range]).splitlines():
            if not line.strip():
                continue
            status, path = line.split("\t", 1)
            diff.files[_decode_git_path(path).replace("\\", "/")] = status[0]
        for line in _git(["diff", "--numstat", "--no-renames", diff.range]).splitlines():
            if not line.strip():
                continue
            added, deleted, path = line.split("\t", 2)
            diff.lines[_decode_git_path(path).replace("\\", "/")] = (
                int(added) if added.isdigit() else 0,
                int(deleted) if deleted.isdigit() else 0,
            )
        if local:
            for rel in _git(["ls-files", "--others", "--exclude-standard"]).splitlines():
                rel = rel.strip().replace("\\", "/")
                if not rel:
                    continue
                diff.files[rel] = "A"
                full = REPO_ROOT / rel
                count = len(read_text(full).splitlines()) if full.exists() else 0
                diff.lines[rel] = (count, 0)
    except RuntimeError as exc:
        diff.available = False
        diff.reason = f"git diff 失败：{exc}"
    return diff


def _labels(args: argparse.Namespace) -> set[str]:
    raw = args.labels or os.environ.get("PR_LABELS", "")
    return {label.strip() for label in raw.split(",") if label.strip()}


def _enforce(args: argparse.Namespace) -> bool:
    """标签类规则只在 PR 环境强制执行；本地降级为警告，免得 pre-commit 无法自证。"""
    if getattr(args, "enforce", False):
        return True
    return os.environ.get("CI_ENFORCE_LABELS") == "1"


def scope_whitelist(result: CheckResult, diff: Diff, args: argparse.Namespace) -> None:
    """rule: CI-DIF-001 —— 只允许约定的顶层目录与根级配置文件。"""
    if not diff.available:
        return
    for path in diff.files:
        parts = Path(path).parts
        top = parts[0] if parts else path
        if len(parts) == 1 and path not in ALLOWED_ROOT_FILES:
            result.violations.append(
                Violation("CI-DIF-001", path, f"根级文件 {path} 不在白名单；新增需在 ci-spec 5.1 登记")
            )
        elif len(parts) > 1 and top not in ALLOWED_TOP:
            result.violations.append(
                Violation("CI-DIF-001", path, f"未登记的顶层目录 {top}/；新增目录需规则负责人批准")
            )


def diff_budget(result: CheckResult, diff: Diff, args: argparse.Namespace) -> None:
    """rule: CI-DIF-002 —— 核心代码 ≤400 行警告、>800 行失败，测试与文档不计。"""
    if not diff.available:
        return
    total = 0
    for path, (added, deleted) in diff.lines.items():
        if not path.startswith(CORE_PREFIXES):
            continue
        if path.endswith((".md", ".json", ".yml", ".yaml")):
            continue
        total += added + deleted
    result.notes.append(f"核心代码改动 {total} 行")
    if total > 800:
        result.violations.append(
            Violation("CI-DIF-002", "diff", f"核心代码改动 {total} 行 > 800，拆分 PR 或申请重构例外")
        )
    elif total > 400:
        result.violations.append(
            Violation("CI-DIF-002", "diff", f"核心代码改动 {total} 行 > 400（警告）", severity="warning")
        )


def rule_files(result: CheckResult, diff: Diff, args: argparse.Namespace) -> None:
    """rule: CI-DIF-003 —— 规则/workflow 文件改动必须带 ci-rule-change 标签。

    rule: CI-MEM-003 —— sanitizer 抑制文件与规则文件同权重，改动需同样批准。
    """
    if not diff.available:
        return
    touched = [p for p in diff.files if any(fnmatch.fnmatch(p, pat) for pat in RULE_PATTERNS)]
    if not touched:
        return
    result.notes.append(f"规则文件改动：{len(touched)} 个")
    if "ci-rule-change" not in _labels(args):
        severity = "error" if _enforce(args) else "warning"
        for path in touched:
            result.violations.append(
                Violation("CI-DIF-003", path, "改动规则文件但缺少 ci-rule-change 标签与规则负责人 review", severity)
            )


def protected_tests(result: CheckResult, diff: Diff, args: argparse.Namespace) -> None:
    """rule: CI-DIF-004 —— 删除或禁用受保护测试必须声明行为变更。

    rule: CI-TST-006 —— 受保护测试由注册表中的 protected 标签定义。
    """
    if not diff.available:
        return
    registry = read_text(REPO_ROOT / "docs/testing.md") if (REPO_ROOT / "docs/testing.md").exists() else ""
    protected_scopes = [
        row[0]
        for row in iter_table_rows(registry, "Scope")
        if len(row) >= 5 and "protected" in row[4]
    ]
    approved = "behavior-change" in _labels(args)
    for path, status in diff.files.items():
        if not any(path.startswith(scope.rstrip("/") + "/") or path == scope for scope in protected_scopes):
            continue
        reasons = []
        if status == "D":
            reasons.append("删除受保护测试文件")
        patch = diff.text_of(path, unified=0)
        added = [ln for ln in patch.splitlines() if ln.startswith("+") and not ln.startswith("+++")]
        if any(SKIP_MARKERS.search(ln) for ln in added):
            reasons.append("新增 skip/禁用标记")
        removed = [ln[1:] for ln in patch.splitlines() if ln.startswith("-") and not ln.startswith("---")]
        if any(ASSERT_MARKERS.search(ln) for ln in removed):
            reasons.append("删除断言")
        if reasons and not approved:
            severity = "error" if _enforce(args) else "warning"
            result.violations.append(
                Violation("CI-DIF-004", path, "；".join(reasons) + "，需要 behavior-change 标签", severity)
            )


def sensitive_files(result: CheckResult, diff: Diff, args: argparse.Namespace) -> None:
    """rule: CI-DIF-005 —— 禁止凭据文件与 >2 MB 的单个新增文件。"""
    if not diff.available:
        return
    limit = 2 * 1024 * 1024
    for path, status in diff.files.items():
        if any(fnmatch.fnmatch(path, pat) for pat in SENSITIVE_PATTERNS):
            result.violations.append(Violation("CI-DIF-005", path, "疑似凭据/私钥文件，禁止入库"))
            continue
        if status == "A":
            full = REPO_ROOT / path
            if full.exists() and full.stat().st_size > limit:
                result.violations.append(
                    Violation("CI-DIF-005", path, f"新增文件 {full.stat().st_size} bytes > 2 MB")
                )


def generated_paths(result: CheckResult, diff: Diff, args: argparse.Namespace) -> None:
    """rule: CI-DIF-006 —— 构建产物、依赖树、缓存不得进入 Diff。"""
    if not diff.available:
        return
    for path in diff.files:
        if any(fnmatch.fnmatch(path, pat) for pat in GENERATED_PATTERNS):
            result.violations.append(
                Violation("CI-DIF-006", path, "生成物/依赖目录不应入库；检查 .gitignore")
            )


def load_waivers(result: CheckResult) -> list[dict]:
    """rule: CI-DIF-007 —— 例外字段齐全、审批人分离、到期即失效。

    rule: CI-RUL-002 —— 例外不续期，到期后只能重新申请。
    """
    path = REPO_ROOT / WAIVER_FILE
    if not path.exists():
        result.notes.append("waivers.json 不存在，视为无例外")
        return []
    try:
        payload = json.loads(read_text(path))
    except json.JSONDecodeError as exc:
        result.violations.append(Violation("CI-DIF-007", WAIVER_FILE, f"JSON 解析失败：{exc}"))
        return []
    if payload.get("version") != 1:
        result.violations.append(Violation("CI-DIF-007", WAIVER_FILE, "version 必须为 1"))
    rules = set(declared_rules(read_text(REPO_ROOT / SPEC_FILE))) if (REPO_ROOT / SPEC_FILE).exists() else set()
    today = date.today()
    valid: list[dict] = []
    for index, item in enumerate(payload.get("waivers", [])):
        missing = [k for k in ("rule", "scope", "owner", "approver", "reason", "created", "expires") if not item.get(k)]
        if missing:
            result.violations.append(
                Violation("CI-DIF-007", f"{WAIVER_FILE}#{index}", f"缺少字段 {missing}")
            )
            continue
        if item["approver"] == item["owner"]:
            result.violations.append(
                Violation("CI-DIF-007", f"{WAIVER_FILE}#{index}", "approver 不能等于 owner（自批无效）")
            )
            continue
        if rules and item["rule"] not in rules:
            result.violations.append(
                Violation("CI-DIF-007", f"{WAIVER_FILE}#{index}", f"规则 {item['rule']} 不在规则目录")
            )
            continue
        try:
            created = date.fromisoformat(item["created"])
            expires = date.fromisoformat(item["expires"])
        except ValueError:
            result.violations.append(
                Violation("CI-DIF-007", f"{WAIVER_FILE}#{index}", "created/expires 必须是 YYYY-MM-DD")
            )
            continue
        if expires > created + timedelta(days=90):
            result.violations.append(
                Violation("CI-DIF-007", f"{WAIVER_FILE}#{index}", "有效期超过 90 天")
            )
            continue
        if expires < today:
            result.notes.append(f"例外已过期（{item['rule']} / {item['scope']} / {expires}），不再生效")
            continue
        valid.append(item)
    result.notes.append(f"生效例外 {len(valid)} 条")
    return valid


def apply_waivers(violations: list[Violation], waivers: list[dict]) -> None:
    for violation in violations:
        if violation.severity != "error":
            continue
        for waiver in waivers:
            if waiver["rule"] != violation.rule:
                continue
            if fnmatch.fnmatch(violation.path, waiver["scope"]) or fnmatch.fnmatch(
                violation.path, waiver["scope"] + "/*"
            ):
                violation.waived = True
                break


def rule_owners(result: CheckResult, diff: Diff, args: argparse.Namespace) -> None:
    """rule: CI-RUL-001 —— CODEOWNERS 必须覆盖规则、workflow 与文档目录。"""
    path = REPO_ROOT / CODEOWNERS
    if not path.exists():
        result.violations.append(Violation("CI-RUL-001", CODEOWNERS, "CODEOWNERS 缺失"))
        return
    text = read_text(path)
    for required in (".github/", "scripts/ci/", "docs/"):
        if required not in text:
            result.violations.append(
                Violation("CI-RUL-001", CODEOWNERS, f"缺少 {required} 的负责人规则")
            )


def retry_budget(result: CheckResult, diff: Diff, args: argparse.Namespace) -> None:
    """rule: CI-GAT-003 —— 单 job 最多 2 次，第 3 次必须定位原因。"""
    attempt = os.environ.get("GITHUB_RUN_ATTEMPT")
    if not attempt or not attempt.isdigit():
        return
    result.notes.append(f"当前运行是第 {attempt} 次尝试")
    if int(attempt) > 2:
        result.violations.append(
            Violation("CI-GAT-003", "run", f"第 {int(attempt)} 次尝试超过上限 2，必须定位原因或隔离")
        )


CHECKS = [
    scope_whitelist,
    diff_budget,
    rule_files,
    protected_tests,
    sensitive_files,
    generated_paths,
    rule_owners,
    retry_budget,
]


def run(base: str | None = None, labels: str = "", enforce: bool = False) -> CheckResult:
    args = argparse.Namespace(base=base, labels=labels, enforce=enforce)
    result = CheckResult("diff")
    diff = collect_diff(base)
    if not diff.available:
        result.skipped = True
        result.skip_reason = diff.reason
        return result
    if diff.reason:
        result.notes.append(diff.reason)
    result.notes.append(f"改动文件 {len(diff.files)} 个")
    for check in CHECKS:
        check(result, diff, args)
    waivers = load_waivers(result)
    apply_waivers(result.violations, waivers)
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="改动闸门")
    parser.add_argument("--base", default=None, help="比较基线，默认取 GITHUB_BASE_REF 或 origin/main")
    parser.add_argument("--labels", default="", help="逗号分隔的 PR 标签，默认读 PR_LABELS")
    parser.add_argument("--enforce", action="store_true", help="强制执行标签规则（PR 环境）")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    guard_console()
    result = run(args.base, args.labels, args.enforce)
    emit(result, args.json)
    write_report([result], stem="report-diff")
    return 0 if result.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
