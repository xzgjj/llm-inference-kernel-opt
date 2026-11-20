"""env — 环境体检。

GPU/内存/OS/CPU/磁盘探测全部复用 docs/规划/tools/probe_capabilities.py 的
build_report()，本模块只补充工具链（编译器/CMake/Ninja/gdb/Python）与 vendor 完整性，
不写第二套 GPU/内存探测。报告落 build/env.json。
"""

from __future__ import annotations

import json
import os
import platform
import shutil
import sys

from dev_commands import common

NAME = "env"
HELP = "环境体检：工具链/GPU/内存/OS 同源探测，写 build/env.json"

REQUIRED_VENDOR_FIELDS = ("name", "version", "sha256", "source")


def _detect_compiler() -> dict:
    for name, version_args in (
        ("cl", None),  # MSVC 版本号在 banner（stderr）里
        ("gcc", ("--version",)),
        ("clang", ("--version",)),
    ):
        exe = shutil.which(name)
        if exe:
            argv = [exe] + list(version_args or [])
            rc, out, err = common.run_cmd(argv, timeout=5)
            head = (out or err).splitlines()
            return {"name": name, "path": exe, "version": head[0] if head else None, "rc": rc}
    return {"name": None, "path": None, "version": None}


def add_arguments(parser) -> None:
    parser.add_argument("--out", default=None, help="自定义报告输出路径（默认 build/env.json）")


def run(args) -> int:
    root = common.repo_root()
    notes: list[str] = []

    # —— 同源探测：直接调用 probe_capabilities.build_report() —— #
    probe, probe_err = common.load_probe()
    report = None
    if probe is None:
        notes.append(probe_err or "probe 模块不可用，GPU/内存字段置空（降级）")
    else:
        try:
            report = probe.build_report()
        except Exception as exc:  # 致命：基础系统信息都读不到
            notes.append(f"probe.build_report() 致命失败: {type(exc).__name__}: {exc}")

    toolchain = {
        "python": {"version": sys.version.split()[0], "executable": sys.executable},
        "os_name": os.name,
        "platform": platform.platform(),
        "cmake": common.tool_version("cmake"),
        "ninja": common.tool_version("ninja"),
        "gdb": common.tool_version("gdb"),
        "compiler": _detect_compiler(),
    }
    vendor = common.vendor_status(root)

    payload = {
        "schema_version": "1.0",
        "timestamp": common.now_iso(),
        "probe_source": "docs/规划/tools/probe_capabilities.py（同源复用 build_report()，未另写 GPU/内存探测）",
        "probe": report,
        "toolchain": toolchain,
        "vendor": vendor,
        "notes": notes,
    }

    out_path = root / (args.out or "build/env.json")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8"
    )

    if getattr(args, "json", False):
        common.print_json(payload)
    else:
        gpus = common.gpu_free_bytes(report)
        print(f"Python {toolchain['python']['version']}  on {toolchain['platform']}")
        print(
            f"cmake: {'有' if toolchain['cmake']['found'] else '缺'}  "
            f"ninja: {'有' if toolchain['ninja']['found'] else '缺'}  "
            f"gdb: {'有' if toolchain['gdb']['found'] else '缺'}  "
            f"compiler: {toolchain['compiler']['name'] or '未检测到'}"
        )
        if gpus:
            for gpu in gpus:
                print(f"GPU {gpu['index']}: {gpu['name']}  free="
                      f"{common.human_size(gpu['vram_free_bytes'])}")
        else:
            print("GPU: 未探测到（见 probe.notes）")
        print(f"报告已写入: {out_path.relative_to(root)}")
        for note in notes:
            print(f"note: {note}")

    # probe 致命失败 → 环境不可用；部分字段为 null 仍算成功（probe 设计允许）
    return common.EXIT_OK if report is not None else common.EXIT_ENV_UNAVAILABLE
