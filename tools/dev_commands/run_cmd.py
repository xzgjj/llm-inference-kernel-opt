"""run — 构建后启动服务。

- 无二进制：提示并退出 2（环境不可用）
- --attach：轮询 /health 直到就绪，前台跟随日志；Ctrl-C 干净退出
- 启动失败：打印最后 30 行日志与进程退出码
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

from dev_commands import common

NAME = "run"
HELP = "启动服务；--attach 轮询 /health 并跟随日志；无二进制退出 2"

HEALTH_TIMEOUT_S = 30
HEALTH_INTERVAL_S = 1


def find_binary(root: Path) -> Path | None:
    """在 build/bin 与 build/<preset>/bin 下找可执行文件。"""
    candidates: list[Path] = []
    for pattern in ("build/bin/*", "build/*/bin/*"):
        candidates.extend(root.glob(pattern))
    for path in sorted(candidates):
        if not path.is_file():
            continue
        if os.name == "nt":
            if path.suffix.lower() == ".exe":
                return path
        elif os.access(path, os.X_OK):
            return path
    return None


def _poll_health(port: int, timeout: int) -> tuple[bool, str]:
    url = f"http://127.0.0.1:{port}/health"
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=2) as resp:
                if resp.status < 500:
                    return True, f"health OK ({resp.status})"
        except (urllib.error.URLError, OSError):
            pass
        time.sleep(HEALTH_INTERVAL_S)
    return False, f"等待 /health 超时（{timeout}s）"


def add_arguments(parser) -> None:
    parser.add_argument("--model", default=None, help="模型路径/名称")
    parser.add_argument("--port", type=int, default=8080, help="服务端口（默认 8080）")
    parser.add_argument("--attach", action="store_true", help="轮询 /health 就绪后前台跟随日志")


def run(args) -> int:
    root = common.repo_root()
    binary = find_binary(root)
    if binary is None:
        msg = "未找到可执行二进制（build/bin/ 为空），请先 build"
        print(msg, file=sys.stderr)
        if getattr(args, "json", False):
            common.print_json({"skipped": True, "reason": msg})
        return common.EXIT_ENV_UNAVAILABLE

    argv = [str(binary)]
    if args.model:
        argv += ["--model", args.model]
    argv += ["--port", str(args.port)]

    server_log = common.logs_dir() / "server" / f"server-{common.utc_ts()}.log"
    server_log.parent.mkdir(parents=True, exist_ok=True)
    tail: list[str] = []

    print(f"启动: {' '.join(argv)}")
    proc = subprocess.Popen(
        argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace", bufsize=1,
    )

    try:
        if args.attach:
            ready, reason = _poll_health(args.port, HEALTH_TIMEOUT_S)
            if not ready:
                assert proc.stdout is not None
                for line in proc.stdout:
                    tail.append(line.rstrip())
                    if len(tail) > 30:
                        tail.pop(0)
                proc.wait()
                print(f"启动失败: {reason}；进程退出码 {proc.returncode}", file=sys.stderr)
                print("最后 30 行日志：", file=sys.stderr)
                print("\n".join(tail[-30:]), file=sys.stderr)
                return common.EXIT_ENV_UNAVAILABLE
            print(reason)

        assert proc.stdout is not None
        with server_log.open("w", encoding="utf-8") as log_fh:
            for line in proc.stdout:
                line = line.rstrip()
                tail.append(line)
                if len(tail) > 30:
                    tail.pop(0)
                log_fh.write(line + "\n")
                log_fh.flush()
                print(line)
        proc.wait()
    except KeyboardInterrupt:
        print("\n收到 Ctrl-C，停止服务…")
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        return common.EXIT_OK

    if proc.returncode != 0:
        print(f"进程退出码 {proc.returncode}；最后 30 行日志：", file=sys.stderr)
        print("\n".join(tail[-30:]), file=sys.stderr)
        print(f"日志: {server_log.relative_to(root)}", file=sys.stderr)
        return common.EXIT_ENV_UNAVAILABLE

    print(f"服务正常退出；日志: {server_log.relative_to(root)}")
    return common.EXIT_OK
