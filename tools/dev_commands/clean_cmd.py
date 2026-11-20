"""clean — 清理构建残留。

- 默认 dry-run：只打印将删清单与体积；--yes 才执行
- vendor/、models/、源码目录默认不在范围；vendor 需显式 --vendor 且二次确认
- 分类：--build（build/，保留 ci-reports）/ --cache / --logs / --all
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

from dev_commands import common

NAME = "clean"
HELP = "清理构建残留；默认 dry-run，--yes 才删；vendor/models 默认排除"

# 永远不删的目录名（兜底保险）
PROTECTED_NAMES = {"vendor", "models", "src", "tests", "docs", "scripts", "tools", ".git"}


def _build_targets(root: Path) -> list[Path]:
    build = root / "build"
    if not build.exists():
        return []
    return [child for child in build.iterdir() if child.name != "ci-reports"]


def _cache_targets(root: Path) -> list[Path]:
    # 缓存只扫 tools/ 与 build/；docs/、src/ 等源码目录不进清理范围
    targets: list[Path] = []
    for base in ("tools", "build"):
        base_dir = root / base
        if not base_dir.exists():
            continue
        targets += [p for p in base_dir.glob("**/__pycache__") if p.is_dir()]
        targets += [p for p in base_dir.glob("**/*.py[cod]") if p.is_file()]
    targets += list(root.glob("build/**/.ninja_log"))
    targets += list(root.glob("build/**/.ninja_deps"))
    targets += [p for p in root.glob("build/**/CMakeFiles") if p.is_dir()]
    targets += list(root.glob("build/**/CMakeCache.txt"))
    return [p for p in targets if p.exists()]


def _logs_targets(root: Path) -> list[Path]:
    logs = root / "logs"
    if not logs.exists():
        return []
    return list(logs.iterdir())


def _collect(root: Path, args) -> list[Path]:
    targets: list[Path] = []
    if args.all or args.build:
        targets += _build_targets(root)
    if args.all or args.cache:
        targets += _cache_targets(root)
    if args.all or args.logs:
        targets += _logs_targets(root)
    if args.vendor:
        vendor = root / "vendor"
        if vendor.exists():
            targets.append(vendor)
    # 去重 + 兜底保护：受保护目录名绝不进删除清单
    unique: list[Path] = []
    seen: set[Path] = set()
    for path in targets:
        resolved = path.resolve()
        if resolved in seen:
            continue
        seen.add(resolved)
        if path.name in PROTECTED_NAMES and path != root / "vendor":
            continue
        unique.append(path)
    return unique


def _remove(path: Path) -> None:
    if path.is_dir():
        shutil.rmtree(path, ignore_errors=True)
    else:
        path.unlink(missing_ok=True)


def add_arguments(parser) -> None:
    parser.add_argument("--build", action="store_true", help="清理 build/（保留 ci-reports）")
    parser.add_argument("--cache", action="store_true", help="清理缓存（__pycache__/ninja/CMakeFiles）")
    parser.add_argument("--logs", action="store_true", help="清理 logs/")
    parser.add_argument("--all", action="store_true", help="build + cache + logs")
    parser.add_argument("--dry-run", action="store_true", help="只打印清单与体积，不删除（默认行为）")
    parser.add_argument("--vendor", action="store_true", help="额外清理 vendor/（需二次确认）")
    parser.add_argument("--yes", action="store_true", help="真正执行删除；默认 dry-run")


def run(args) -> int:
    root = common.repo_root()
    targets = _collect(root, args)

    # vendor 二次确认：非 TTY 下未显式确认则拒绝
    if args.vendor and args.yes:
        if common.is_tty():
            answer = input("确认删除 vendor/ 及其全部内容？输入 yes 继续: ").strip()
            if answer != "yes":
                print("已取消：vendor/ 未删除", file=sys.stderr)
                return common.EXIT_ARG_ERROR
        else:
            print("非 TTY 环境下 vendor/ 删除需交互式二次确认，已跳过", file=sys.stderr)
            targets = [t for t in targets if t.name != "vendor"]

    if not targets:
        print("没有可清理的目标（models/、vendor/、源码目录默认不在范围）")
        if getattr(args, "json", False):
            common.print_json({"dry_run": not args.yes, "targets": [], "total_size": 0})
        return common.EXIT_OK

    entries = []
    total = 0
    for path in targets:
        size = common.dir_size(path) if path.is_dir() else (path.stat().st_size if path.is_file() else 0)
        total += size
        entries.append({"path": str(path.relative_to(root)), "size_bytes": size})

    if not args.yes:
        print(f"[dry-run] 将清理 {len(entries)} 项，合计 {common.human_size(total)}（加 --yes 执行）：")
        for entry in entries:
            print(f"  {common.human_size(entry['size_bytes']):>10s}  {entry['path']}")
        if getattr(args, "json", False):
            common.print_json({"dry_run": True, "targets": entries, "total_size": total})
        return common.EXIT_OK

    for path in targets:
        _remove(path)
    print(f"已清理 {len(entries)} 项，合计 {common.human_size(total)}")
    if getattr(args, "json", False):
        common.print_json({"dry_run": False, "targets": entries, "total_size": total})
    return common.EXIT_OK
