"""范围复核：非目标形态与测试登记扫描（4-G5）。

非目标：分布式通信、自定义 CUDA 算子、训练通路、PD 分离。
测试登记：tests 下的 .cc/.py 必须出现在 tests/CMakeLists.txt 或被目录级 discover 覆盖。
"""
from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FORBIDDEN_TERMS = {
    "nccl": "分布式通信",
    "torch::": "训练通路",
    "disagg": "PD 分离",
    "pd_disaggregation": "PD 分离",
    "cudaMalloc": "自定义 CUDA 算子",
    "cudaLaunchKernel": "自定义 CUDA 算子",
}
FORBIDDEN_DIRS = ("src/cuda", "src/training", "src/disagg", "src/pd")
TEST_DIRS = ("tests/unit", "tests/contract", "tests/integration", "tests/e2e")


def source_files() -> list[Path]:
    files: list[Path] = []
    for directory in ("src", "include"):
        root = REPO / directory
        if root.exists():
            files.extend(path for path in root.rglob("*") if path.suffix in {".cpp", ".hpp", ".cc", ".h"})
    return files


def scan_terms() -> list[str]:
    failures = []
    for path in source_files():
        text = path.read_text(encoding="utf-8", errors="replace")
        for term, label in FORBIDDEN_TERMS.items():
            if term in text:
                failures.append(f"{path.relative_to(REPO)} 命中 {term}（{label}）")
    for directory in FORBIDDEN_DIRS:
        if (REPO / directory).exists():
            failures.append(f"非目标目录存在: {directory}")
    return failures


def test_files() -> list[Path]:
    files: list[Path] = []
    for directory in TEST_DIRS:
        root = REPO / directory
        if root.exists():
            files.extend(path for path in root.rglob("*") if path.suffix in {".cc", ".py"}
                         and path.name != "__init__.py")
    return files


def scan_tests() -> list[str]:
    registered = (REPO / "tests" / "CMakeLists.txt").read_text(encoding="utf-8")
    failures = []
    for path in test_files():
        if path.name.endswith("_kit.py"):
            continue
        if path.name not in registered:
            failures.append(f"测试未登记: {path.relative_to(REPO)}")
    return failures


def scan_gates() -> list[str]:
    entries = ["scripts/ci/run_ci.py", "scripts/ci/check_docs.py",
               "scripts/ci/check_tests.py", "scripts/ci/check_code.py"]
    return [f"闸门入口缺失: {entry}" for entry in entries if not (REPO / entry).exists()]


def main() -> int:
    failures = scan_terms() + scan_tests() + scan_gates()
    if failures:
        for failure in failures:
            print(f"[scope] {failure}")
        return 1
    print(f"[scope] PASS：{len(source_files())} 个源文件、{len(test_files())} 个测试文件均登记")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
