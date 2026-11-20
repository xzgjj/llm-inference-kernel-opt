"""debug — gdb 调试包装。

- 生成 gdb 脚本：set pagination off / catch throw / handle SIGSEGV stop /
  崩溃 thread apply all bt full / 按需打印 Batch/Sequence/slot 状态
- 日志落 build/logs/gdb-<ts>.log；--core 走 core dump
- Windows 侧检测不到 gdb：明确提示"请在 WSL 内执行"并打印可复制的 wsl 命令行，退出 2
- 无目标二进制：提示并退出 2
"""

from __future__ import annotations

import os
import re
import shutil
import sys
from pathlib import Path

from dev_commands import common

NAME = "debug"
HELP = "生成 gdb 脚本调试；Windows 无 gdb 时给出 WSL 命令行，退出 2"


def _default_wsl_distro() -> str | None:
    rc, out, _ = common.run_cmd(["wsl.exe", "-l", "-q"], timeout=8)
    if rc != 0:
        return None
    for line in out.splitlines():
        line = line.strip().strip("\r")
        if line:
            return line
    return None


def _win_to_wsl_path(path: Path) -> str:
    """Windows 路径转 WSL /mnt/<drive>/... 路径。"""
    text = str(path.resolve())
    match = re.match(r"^([A-Za-z]):[\\/](.*)$", text)
    if not match:
        return text
    drive, rest = match.group(1).lower(), match.group(2)
    return f"/mnt/{drive}/" + rest.replace("\\", "/")


def _gdb_script(where: str | None) -> str:
    lines = [
        "set pagination off",
        "set confirm off",
        "catch throw",
        "handle SIGSEGV stop print nopass",
    ]
    if where:
        lines.append(f"break {where}")
    lines += [
        "run",
        "bt full",
        "thread apply all bt full",
        "info threads",
        "# Batch / Sequence / slot 状态（符号存在时取消注释按需打印）:",
        "# p g_scheduler",
        "# p g_sequence_table",
        "# p g_kv_block_table",
        "quit",
    ]
    return "\n".join(lines) + "\n"


def add_arguments(parser) -> None:
    parser.add_argument("--bin", required=True, help="目标二进制路径")
    parser.add_argument("--args", nargs="*", default=[], help="传给二进制的参数")
    parser.add_argument("--break", dest="where", default=None, help="断点位置（函数/行号）")
    parser.add_argument("--core", default=None, help="core dump 文件，走事后调试")


def run(args) -> int:
    root = common.repo_root()
    binary = Path(args.bin)
    if not binary.is_absolute():
        binary = root / binary
    if not binary.exists():
        msg = f"目标二进制不存在: {binary}"
        print(msg, file=sys.stderr)
        if getattr(args, "json", False):
            common.print_json({"skipped": True, "reason": msg})
        return common.EXIT_ENV_UNAVAILABLE

    gdb = shutil.which("gdb")

    # Windows 侧无 gdb：不静默失败，给出可直接复制的 WSL 命令行
    if os.name == "nt" and not gdb:
        distro = _default_wsl_distro() or "<distro>"
        wsl_cwd = _win_to_wsl_path(root)
        wsl_bin = _win_to_wsl_path(binary)
        inner = f"cd {wsl_cwd} && gdb --args {wsl_bin} {' '.join(args.args)}".strip()
        cmd = f'wsl.exe -d {distro} -- bash -lc "{inner}"'
        print("请在 WSL 内执行（Windows 侧未检测到 gdb）：", file=sys.stderr)
        print(cmd, file=sys.stderr)
        if getattr(args, "json", False):
            common.print_json({"skipped": True, "reason": "Windows 无 gdb", "wsl_command": cmd})
        return common.EXIT_ENV_UNAVAILABLE

    if not gdb:
        msg = "环境不可用: 未找到 gdb"
        print(msg, file=sys.stderr)
        return common.EXIT_ENV_UNAVAILABLE

    script_path = common.build_logs_dir() / f"gdb-script-{common.utc_ts()}.gdb"
    script_path.write_text(_gdb_script(args.where), encoding="utf-8")
    log_path = common.build_logs_dir() / f"gdb-{common.utc_ts()}.log"

    if args.core:
        core = Path(args.core)
        if not core.is_absolute():
            core = root / core
        if not core.exists():
            print(f"core dump 不存在: {core}", file=sys.stderr)
            return common.EXIT_ENV_UNAVAILABLE
        argv = [gdb, "-c", str(core), str(binary)]
    else:
        argv = [gdb, "-x", str(script_path), "--args", str(binary), *args.args]

    rc, out, err = common.run_cmd(argv, timeout=300)
    log_path.write_text(out + ("\n--- stderr ---\n" + err if err else ""), encoding="utf-8")

    print(f"gdb 日志: {log_path.relative_to(root)}")
    if rc != 0:
        print(f"gdb 退出码 {rc}", file=sys.stderr)
        return common.EXIT_ENV_UNAVAILABLE
    return common.EXIT_OK
