"""build — CMake + Ninja 包装。

- 无 CMakeLists.txt：打印"未找到 CMakeLists.txt，跳过构建"并退出 0（优雅降级）
- 进度解析 ninja `[x/y]`：百分比 + 已用时 + ETA + 当前目标；非 TTY 每 5% 打一行
- 全量日志落 build/logs/build-<ts>.log
- 失败：首个 `error:` 及上下文 10 行打到终端，写 build/logs/last-error.txt，退出 4
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

from dev_commands import common

NAME = "build"
HELP = "包装 CMake+Ninja（进度/ETA/日志）；无 CMakeLists 时跳过退出 0"

PROGRESS_RE = re.compile(r"^\[(\d+)/(\d+)\]")
ERROR_RE = re.compile(r"error:", re.IGNORECASE)

PRESET_BUILD_TYPE = {
    "dev": "RelWithDebInfo",
    "debug": "Debug",
    "release": "Release",
    "asan": "Debug",
}


def parse_progress(line: str) -> dict | None:
    """解析 ninja 进度行 `[cur/total] ...`；非进度行返回 None。"""
    match = PROGRESS_RE.match(line.strip())
    if not match:
        return None
    cur, total = int(match.group(1)), int(match.group(2))
    percent = (cur / total * 100.0) if total else 0.0
    target = line.strip().split("]", 1)[1].strip()
    return {"cur": cur, "total": total, "percent": percent, "target": target}


def extract_error(lines: list[str], context: int = 10) -> list[str]:
    """取首个含 `error:` 的行及其后 context 行；找不到则返回空列表。"""
    for idx, line in enumerate(lines):
        if ERROR_RE.search(line):
            return lines[idx: idx + context + 1]
    return []


def _format_eta(seconds: float | None) -> str:
    if seconds is None or seconds <= 0:
        return "ETA ?"
    return f"ETA {seconds:.0f}s"


def add_arguments(parser) -> None:
    parser.add_argument("--preset", choices=["dev", "debug", "release", "asan"], default="dev")
    parser.add_argument("--target", default=None, help="只构建指定目标")
    parser.add_argument("-j", type=int, default=None, help="并行任务数")
    parser.add_argument("--verbose", action="store_true", help="详细输出")


def _run_streamed(argv: list[str], log_path: Path, start: float) -> tuple[int, list[str]]:
    """流式执行子进程，全量写日志；返回 (rc, 全部行)。"""
    all_lines: list[str] = []
    last_bucket = -1
    log_path.parent.mkdir(parents=True, exist_ok=True)

    with log_path.open("w", encoding="utf-8") as log_fh:
        proc = subprocess.Popen(
            argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, encoding="utf-8", errors="replace", bufsize=1,
        )
        assert proc.stdout is not None
        for line in proc.stdout:
            line = line.rstrip("\n")
            all_lines.append(line)
            log_fh.write(line + "\n")
            log_fh.flush()

            progress = parse_progress(line)
            if progress:
                elapsed = time.time() - start
                rate = progress["cur"] / elapsed if elapsed > 0 else 0
                eta = (progress["total"] - progress["cur"]) / rate if rate > 0 else None
                if common.is_tty():
                    print(f"[{progress['percent']:5.1f}%] {progress['cur']}/{progress['total']} "
                          f"elapsed {elapsed:.0f}s {_format_eta(eta)}  {progress['target']}")
                else:
                    bucket = int(progress["percent"] // 5)
                    if bucket != last_bucket:
                        last_bucket = bucket
                        print(f"[{progress['percent']:5.1f}%] {progress['cur']}/{progress['total']} "
                              f"elapsed {elapsed:.0f}s {_format_eta(eta)}  {progress['target']}")
            else:
                if common.is_tty():
                    print(line)
        proc.wait()
    return proc.returncode, all_lines


def run(args) -> int:
    root = common.repo_root()

    # 优雅降级：无 CMakeLists.txt 不算构建失败
    if not (root / "CMakeLists.txt").exists():
        msg = "未找到 CMakeLists.txt，跳过构建"
        print(msg)
        if getattr(args, "json", False):
            common.print_json({"skipped": True, "reason": msg})
        return common.EXIT_OK

    cmake = shutil.which("cmake")
    ninja = shutil.which("ninja")
    if not cmake or not ninja:
        msg = f"环境不可用: cmake={'有' if cmake else '缺'} ninja={'有' if ninja else '缺'}"
        print(msg, file=sys.stderr)
        if getattr(args, "json", False):
            common.print_json({"skipped": False, "error": msg})
        return common.EXIT_ENV_UNAVAILABLE

    build_dir = root / "build" / args.preset
    log_path = common.build_logs_dir() / f"build-{common.utc_ts()}.log"
    start = time.time()

    configure = [
        cmake, "-S", str(root), "-B", str(build_dir), "-G", "Ninja",
        f"-DCMAKE_BUILD_TYPE={PRESET_BUILD_TYPE[args.preset]}",
    ]
    build_argv = [cmake, "--build", str(build_dir)]
    if args.target:
        build_argv += ["--target", args.target]
    if args.j:
        build_argv += ["-j", str(args.j)]
    if args.verbose:
        build_argv.append("--verbose")

    rc, lines = _run_streamed(configure, log_path, start)
    if rc == 0:
        rc, lines = _run_streamed(build_argv, log_path, start)

    if rc != 0:
        error_lines = extract_error(lines)
        last_error = common.build_logs_dir() / "last-error.txt"
        last_error.write_text("\n".join(error_lines) or "（未匹配到 error: 行，见全量日志）",
                              encoding="utf-8")
        print("\n构建失败，首个错误上下文：", file=sys.stderr)
        for line in error_lines:
            print(line, file=sys.stderr)
        print(f"全量日志: {log_path.relative_to(root)}；错误摘录: {last_error.relative_to(root)}",
              file=sys.stderr)
        if getattr(args, "json", False):
            common.print_json({"failed": True, "log": str(log_path), "error_lines": error_lines})
        return common.EXIT_BUILD_FAIL

    print(f"构建成功（{args.preset}），日志: {log_path.relative_to(root)}")
    return common.EXIT_OK
