"""覆盖率闸门：总行覆盖率 + 增量覆盖率，两条都必须过。

用 gcovr 产出 JSON（CI 装 gcovr，本地可 pip install gcovr），本脚本只做判定：
统计口径只看 src/，tests/、third_party/、build*/ 一律排除——头文件里的模板与内联
代码会按实例化摊到每个 TU，混进总分会把数值虚高。
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
from pathlib import Path

from ci_common import REPO_ROOT, CheckResult, Violation, emit, guard_console, write_report

SOURCE_PREFIX = "src/"


def run_gcovr(build_dir: str, json_path: Path) -> None:
    gcovr = shutil.which("gcovr")
    if not gcovr:
        raise FileNotFoundError("gcovr 未安装：CI 用 pip install gcovr，本地同样")
    cmd = [
        gcovr, "--root", str(REPO_ROOT), "--object-directory", build_dir,
        "--json", str(json_path),
        # gcovr 的 filter 匹配相对 root 的报告路径；写绝对路径会把全部数据过滤掉。
        "--filter", SOURCE_PREFIX,
        "--exclude", "third_party/",
        "--exclude", "tests/",
        "--exclude-unreachable-branches",
        "--gcov-ignore-parse-errors", "negative_hits.warn_once_per_file",
    ]
    subprocess.run(cmd, cwd=REPO_ROOT, check=True)


def load_lines(json_path: Path) -> dict[str, dict[int, int]]:
    data = json.loads(json_path.read_text(encoding="utf-8"))
    files: dict[str, dict[int, int]] = {}
    root = REPO_ROOT.as_posix()
    for entry in data.get("files", []):
        rel = Path(entry.get("file", "")).as_posix()
        if root in rel:
            rel = rel.split(root, 1)[1].lstrip("/")
        files[rel] = {
            int(line["line_number"]): int(line.get("count") or 0)
            for line in entry.get("lines", [])
            if line.get("count") is not None
        }
    return files


def line(files: dict[str, dict[int, int]], only: set[str] | None = None) -> tuple[int, int]:
    """rule: CI-COV-001 —— src/ 总体行覆盖率：返回 (命中行, 可执行行)。"""
    total = hit = 0
    for path, lines in files.items():
        if not path.startswith(SOURCE_PREFIX):
            continue
        for lineno, count in lines.items():
            if only is not None and f"{path}:{lineno}" not in only:
                continue
            total += 1
            if count > 0:
                hit += 1
    return hit, total


def parse_diff_text(text: str) -> dict[str, set[int]]:
    """从 `git diff -U0` 文本里取 + 侧行号，只留 src/ 下的新增/修改行。"""
    added: dict[str, set[int]] = {}
    current: str | None = None
    for raw in text.splitlines():
        if raw.startswith("+++ b/"):
            current = raw[6:].strip()
            if current != "/dev/null":
                added.setdefault(current, set())
        elif raw.startswith("@@") and current:
            spec = re.search(r"\+\d+(?:,\d+)?", raw)
            if not spec:
                continue
            parts = spec.group(0)[1:].partition(",")
            start = int(parts[0])
            count = int(parts[2]) if parts[2] else 1
            added[current].update(range(start, start + count))
    return {path: lines for path, lines in added.items() if lines}


def diff(files: dict[str, dict[int, int]], base: str) -> tuple[int, int, dict[str, set[int]]]:
    """rule: CI-COV-002 —— 本次改动新增/修改的可执行行覆盖率：返回 (命中, 总数, 改动行)。"""
    proc = subprocess.run(
        ["git", "diff", "--unified=0", f"{base}...HEAD", "--", "src"],
        cwd=REPO_ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or "git diff 失败")
    changed = parse_diff_text(proc.stdout)
    touched = {f"{path}:{lineno}" for path, lines in changed.items() for lineno in lines}
    hit, total = line(files, only=touched)
    return hit, total, changed


def _find_build_dir(preferred: str = "") -> str | None:
    """找真正带插桩产物的构建目录：显式指定优先，否则 build-cov → build 依次探测。"""
    candidates = [preferred] if preferred else ["build-cov", "build"]
    for name in candidates:
        path = REPO_ROOT / name
        if path.is_dir() and any(path.rglob("*.gcda")):
            return name
    return None


def run(base: str = "", build_dir: str = "", min_line: float = 70.0,
        min_diff: float = 80.0) -> CheckResult:
    """给 run_ci.py 调用的入口：无 gcov/无数据时按 SKIP 留痕，不判失败。"""
    result = CheckResult("coverage")
    if not shutil.which("gcovr"):
        result.notes.append("SKIP：gcovr 不可用（Windows/MSVC 无 gcov，由 Linux runner 补跑）")
        return result
    target = _find_build_dir(build_dir)
    if not target:
        result.notes.append("SKIP：没有找到带 --coverage 插桩的 .gcda（需要 -DLLMOPT_COVERAGE=ON 并先跑用例）")
        return result

    report = REPO_ROOT / "build" / "ci-reports"
    report.mkdir(parents=True, exist_ok=True)
    json_path = report / "coverage.json"
    try:
        run_gcovr(target, json_path)
    except (FileNotFoundError, subprocess.CalledProcessError) as exc:
        message = str(exc) if isinstance(exc, FileNotFoundError) else f"gcovr 退出码 {exc.returncode}"
        result.violations.append(Violation("CI-COV-001", target, message))
        return result

    files = load_lines(json_path)
    hit, total = line(files)
    ratio = (100.0 * hit / total) if total else 0.0
    result.notes.append(f"src/ 行覆盖 {hit}/{total} = {ratio:.1f}%（阈值 {min_line}%）")
    if total == 0:
        result.violations.append(
            Violation("CI-COV-001", target, "没有采到任何 src/ 行数据，检查插桩与 gcovr 路径")
        )
    elif ratio < min_line:
        result.violations.append(
            Violation("CI-COV-001", "src/", f"行覆盖率 {ratio:.1f}% 低于阈值 {min_line}%")
        )

    if not base:
        result.notes.append("增量覆盖：未提供基线，SKIP")
        return result
    try:
        diff_hit, diff_total, changed = diff(files, base)
    except RuntimeError as exc:
        result.notes.append(f"增量覆盖：基线不可用，SKIP（{exc}）")
        return result
    diff_ratio = (100.0 * diff_hit / diff_total) if diff_total else 100.0
    result.notes.append(f"增量行覆盖 {diff_hit}/{diff_total} = {diff_ratio:.1f}%（阈值 {min_diff}%）")
    if diff_total and diff_ratio < min_diff:
        result.violations.append(
            Violation("CI-COV-002", base,
                      f"改动行覆盖 {diff_ratio:.1f}% 低于阈值 {min_diff}%：{sorted(changed)[:5]}")
        )
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="覆盖率闸门")
    parser.add_argument("--build-dir", default="")
    parser.add_argument("--min-line", type=float, default=70.0)
    parser.add_argument("--min-diff", type=float, default=80.0)
    parser.add_argument("--base", default="", help="增量覆盖的对比基线，如 origin/main")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    guard_console()
    result = run(args.base, args.build_dir, args.min_line, args.min_diff)
    for note in result.notes:
        print(note)
    emit(result, args.json)
    write_report([result], stem="report-coverage")
    return 0 if result.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
