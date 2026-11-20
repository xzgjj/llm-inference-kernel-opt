"""依赖核对：CMake 实际拉取/链接的第三方 = 清单，且无纸面依赖（4-G0-4 / 4-G4）。

用法：python scripts/ci/check_deps.py [--max 5]
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CMAKE_FILES = ("CMakeLists.txt", "cmake/deps.cmake", "tests/CMakeLists.txt")
KNOWN = {
    "httplib", "nlohmann_json", "spdlog", "doctest", "concurrentqueue", "benchmark",
}
SYSTEM_LIBS = {"psapi", "ws2_32", "llmopt_core", "llmopt_server", "llmopt_coverage",
               "llmopt_warnings", "llama", "ggml"}
REMOVED = {"concurrentqueue", "benchmark"}


def read_all() -> str:
    chunks = []
    for name in CMAKE_FILES:
        path = REPO / name
        if path.exists():
            chunks.append(path.read_text(encoding="utf-8"))
    return "\n".join(chunks)


def declared() -> set[str]:
    text = read_all()
    names = set(re.findall(r"FetchContent_Declare\(\s*([A-Za-z0-9_]+)", text))
    available = re.search(r"FetchContent_MakeAvailable\(([^)]*)\)", text)
    if available:
        names |= {token for token in re.findall(r"[A-Za-z0-9_]+", available.group(1))}
    return names & KNOWN


def linked() -> set[str]:
    text = read_all()
    tokens: set[str] = set()
    for match in re.finditer(r"target_link_libraries\(([^)]*)\)", text, re.S):
        body = match.group(1)
        for token in re.findall(r"[A-Za-z0-9_:\-]+", body):
            base = token.split("::")[0]
            if base in KNOWN:
                tokens.add(base)
    return tokens


def check(max_direct: int = 5) -> tuple[bool, list[str]]:
    messages: list[str] = []
    declared_names = declared()
    linked_names = linked()
    paper = declared_names - linked_names
    if paper:
        messages.append(f"声明但未链接: {sorted(paper)}")
    for name in sorted(REMOVED & (declared_names | linked_names)):
        messages.append(f"禁用依赖仍在: {name}")
    if len(linked_names) > max_direct:
        messages.append(f"直接第三方链接 {len(linked_names)} > {max_direct}: {sorted(linked_names)}")
    return not messages, messages


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="依赖清单核对")
    parser.add_argument("--max", type=int, default=5, help="直接第三方依赖上限")
    args = parser.parse_args(argv)
    ok, messages = check(args.max)
    if not ok:
        for message in messages:
            print(f"[deps] {message}")
        return 1
    print(f"[deps] PASS：直接第三方 {sorted(linked())}，无纸面依赖")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
