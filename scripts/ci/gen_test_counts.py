#!/usr/bin/env python3
# gen_test_counts.py — 测试计数唯一事实来源：从 CTest 注册表生成统计行，写回文档标记块。
# 用法：
#   python scripts/ci/gen_test_counts.py --check          # CI/提交前校验，不一致即退出 1
#   python scripts/ci/gen_test_counts.py --write          # 本地更新 docs/testing.md
#   python scripts/ci/gen_test_counts.py --build-dir build-b --check
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

START = "<!-- test-counts:start -->"
END = "<!-- test-counts:end -->"
DEFAULT_FILES = ("docs/testing.md",)
LABELS = ("fast", "unit", "contract", "integration", "e2e")
TOTAL_RE = re.compile(r"Total Tests:\s*(\d+)")


def count(label: str | None, build_dir: str) -> int:
    cmd = ["ctest", "--test-dir", build_dir, "-N"]
    if label:
        cmd += ["-L", label]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=False)
    except (FileNotFoundError, OSError) as exc:
        raise SystemExit(
            f"ctest 不可用（{exc}）：把 CMake bin 加入 PATH，或用 --build-dir 指向有效构建目录"
        ) from None
    if result.returncode != 0:
        raise SystemExit(f"ctest -N 失败: {result.stderr.strip() or result.stdout.strip()}")
    match = TOTAL_RE.search(result.stdout)
    if not match:
        raise SystemExit(f"无法从 ctest 输出解析用例数（label={label}）")
    return int(match.group(1))


def build_line(build_dir: str) -> str:
    total = count(None, build_dir)
    numbers = {label: count(label, build_dir) for label in LABELS}
    return (
        f"测试基线（脚本生成，勿手改）：`ctest -N` **{total}** 注册；"
        f"`-L fast` **{numbers['fast']}**"
        f"（{numbers['unit']} unit / {numbers['contract']} contract / "
        f"{numbers['integration']} integration）；`-L e2e` {numbers['e2e']}。"
    )


def rewrite(path: Path, line: str, write: bool) -> bool:
    text = path.read_text(encoding="utf-8")
    if START not in text or END not in text:
        raise SystemExit(f"{path}: 缺少统计块标记 {START} … {END}")
    pattern = re.compile(re.escape(START) + r".*?" + re.escape(END), re.S)
    new_text = pattern.sub(f"{START}\n{line}\n{END}", text, count=1)
    if new_text == text:
        return True
    if write:
        path.write_text(new_text, encoding="utf-8")
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--files", nargs="*", default=list(DEFAULT_FILES))
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--check", action="store_true")
    group.add_argument("--write", action="store_true")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    line = build_line(args.build_dir)
    stale = []
    for name in args.files:
        path = root / name
        if not rewrite(path, line, args.write):
            stale.append(name)
    if args.write:
        print(f"[WRITE] {line}")
        print(f"[WRITE] 更新文件: {', '.join(args.files)}")
        return 0
    if stale:
        print(f"[FAIL] 计数漂移: {', '.join(stale)}")
        print(f"       期望行: {line}")
        return 1
    print(f"[PASS] 计数一致: {line}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
