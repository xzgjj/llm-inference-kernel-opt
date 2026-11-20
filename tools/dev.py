#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""dev.py — llm-inference-kernel-opt 开发工具链单入口（v0.1）。

子命令在 dev_commands/ 目录下自动发现：新增命令只需放一个 <name>.py，
暴露 NAME / HELP / add_arguments(parser) / run(args) -> int，不改本文件。

退出码：0 成功 / 1 测试失败 / 2 环境不可用 / 3 参数错误 / 4 构建失败。
零第三方依赖；可选工具缺失时各命令自行降级，不允许 ImportError 直接崩。
"""

from __future__ import annotations

import argparse
import importlib
import pkgutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from dev_commands import common  # noqa: E402

# 不参与命令注册的模块名
_PRIVATE_MODULES = {"common"}


def discover_commands() -> dict[str, object]:
    """扫描 dev_commands 包，返回 {NAME: module}。"""
    commands: dict[str, object] = {}
    pkg_dir = Path(__file__).resolve().parent / "dev_commands"
    for info in pkgutil.iter_modules([str(pkg_dir)]):
        if info.name in _PRIVATE_MODULES or info.name.startswith("_"):
            continue
        module = importlib.import_module(f"dev_commands.{info.name}")
        if hasattr(module, "NAME") and hasattr(module, "run"):
            commands[module.NAME] = module
    return commands


class _ArgumentParser(argparse.ArgumentParser):
    """参数错误统一走退出码 3（契约：3 = 参数错误）。"""

    def error(self, message: str) -> None:
        self.print_usage(sys.stderr)
        self.exit(common.EXIT_ARG_ERROR, f"参数错误: {message}\n")


def build_parser(commands: dict[str, object]) -> _ArgumentParser:
    parser = _ArgumentParser(
        prog="dev.py", description="llm-inference-kernel-opt 开发工具链 v0.1"
    )
    parser.add_argument("--list", action="store_true", help="列出全部已注册子命令")
    sub = parser.add_subparsers(dest="command", metavar="<command>")
    for name, module in sorted(commands.items()):
        sub_parser = sub.add_parser(name, help=module.HELP)  # type: ignore[attr-defined]
        module.add_arguments(sub_parser)  # type: ignore[attr-defined]
        # 每个子命令统一支持 --json
        sub_parser.add_argument("--json", action="store_true", help="以 JSON 输出")
    return parser


def main(argv: list[str] | None = None) -> int:
    common.guard_console()
    commands = discover_commands()
    parser = build_parser(commands)
    args = parser.parse_args(argv)

    if args.list:
        for name, module in sorted(commands.items()):
            print(f"{name:12s} {module.HELP}")  # type: ignore[attr-defined]
        return common.EXIT_OK

    if not args.command:
        parser.print_help()
        return common.EXIT_ARG_ERROR

    module = commands[args.command]
    try:
        return int(module.run(args))  # type: ignore[attr-defined]
    except KeyboardInterrupt:
        print("\n中断: 用户取消", file=sys.stderr)
        return common.EXIT_ENV_UNAVAILABLE


if __name__ == "__main__":
    raise SystemExit(main())
