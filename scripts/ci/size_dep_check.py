"""规模与模块核对：自研代码行数、CMake 产物模块、直接依赖数（4-G4）。

模块口径：CMake 里产出的链接单元（add_library / add_executable，不含 tests）。
用法：python scripts/ci/size_dep_check.py [--max-lines 25000] [--max-modules 10]
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CODE_DIRS = ("src", "include")
MAX_LINES_DEFAULT = 25000
MIN_LINES_WARN = 4500


def sloc(path: Path) -> int:
    """非空、非纯注释行；避免把格式空行与注释当产能。"""
    count = 0
    in_block = False
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        if in_block:
            if "*/" in stripped:
                in_block = False
            continue
        if stripped.startswith("/*"):
            in_block = "*/" not in stripped
            continue
        if stripped.startswith("//") or stripped.startswith("#"):
            continue
        count += 1
    return count


def production_lines() -> int:
    return sum(sloc(path) for directory in CODE_DIRS for path in sorted((REPO / directory).rglob("*")
                                                                       if (REPO / directory).exists() else [])
                if path.suffix in {".cpp", ".hpp", ".h", ".cc"})


def module_count() -> int:
    text = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
    targets = re.findall(r"add_(?:library|executable)\(([A-Za-z0-9_]+)", text)
    return len([target for target in targets if not target.startswith("unit_")])


def direct_deps() -> int:
    import sys
    sys.path.insert(0, str(REPO / "scripts" / "ci"))
    import check_deps

    return len(check_deps.linked())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="规模与依赖核对")
    parser.add_argument("--max-lines", type=int, default=MAX_LINES_DEFAULT)
    parser.add_argument("--max-modules", type=int, default=10)
    args = parser.parse_args(argv)
    lines = production_lines()
    modules = module_count()
    deps = direct_deps()
    failures = []
    if lines > args.max_lines:
        failures.append(f"自研代码 {lines} 行 > {args.max_lines}")
    if modules > args.max_modules:
        failures.append(f"模块 {modules} > {args.max_modules}")
    if deps > 5:
        failures.append(f"直接依赖 {deps} > 5")
    for message in failures:
        print(f"[size] {message}")
    if failures:
        return 1
    warning = "（低于止损线，阶段 6 服务化补回）" if lines < MIN_LINES_WARN else ""
    print(f"[size] PASS：{lines} 行 / {modules} 模块 / {deps} 直接依赖 {warning}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
