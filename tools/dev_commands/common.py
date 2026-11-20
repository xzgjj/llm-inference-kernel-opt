"""dev_commands 公共辅助：路径、退出码、日志、JSON 输出、工具探测与 probe 复用。

退出码契约（tools/dev.py 全局统一）：
  0  成功
  1  测试失败
  2  环境不可用（缺工具/缺二进制/探测致命失败）
  3  参数错误
  4  构建失败
"""

from __future__ import annotations

import json
import os
import platform
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

# --------------------------------------------------------------------------- #
# 退出码
# --------------------------------------------------------------------------- #
EXIT_OK = 0
EXIT_TEST_FAIL = 1
EXIT_ENV_UNAVAILABLE = 2
EXIT_ARG_ERROR = 3
EXIT_BUILD_FAIL = 4


def repo_root() -> Path:
    """仓库根目录。测试可用环境变量 LLMOPT_ROOT 重定向到临时目录。"""
    override = os.environ.get("LLMOPT_ROOT")
    if override:
        return Path(override).resolve()
    # common.py 位于 tools/dev_commands/，上溯两级即仓库根
    return Path(__file__).resolve().parents[2]


def guard_console() -> None:
    """保证非 UTF-8 终端（Windows cp936）打印中文不崩。"""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


def utc_ts() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def logs_dir() -> Path:
    path = repo_root() / "logs"
    path.mkdir(parents=True, exist_ok=True)
    return path


def build_logs_dir() -> Path:
    path = repo_root() / "build" / "logs"
    path.mkdir(parents=True, exist_ok=True)
    return path


def write_log(name: str, text: str, under_build: bool = False) -> Path:
    """统一日志落盘：logs/<command>-<UTC时间戳>.log（under_build 时落 build/logs/）。"""
    directory = build_logs_dir() if under_build else logs_dir()
    path = directory / f"{name}-{utc_ts()}.log"
    path.write_text(text, encoding="utf-8")
    return path


def run_cmd(argv: list[str], timeout: int = 10) -> tuple[int, str, str]:
    """运行子进程，返回 (rc, stdout, stderr)；失败不抛异常。"""
    try:
        proc = subprocess.run(
            argv, capture_output=True, text=True, timeout=timeout, check=False
        )
        return proc.returncode, (proc.stdout or "").strip(), (proc.stderr or "").strip()
    except (FileNotFoundError, subprocess.TimeoutExpired, OSError) as exc:
        return 127, "", f"{type(exc).__name__}: {exc}"


def tool_version(name: str, version_args: tuple[str, ...] = ("--version",)) -> dict:
    """探测可选工具是否存在及版本；缺失时降级为 found=False，不抛异常。"""
    exe = shutil.which(name)
    if not exe:
        return {"found": False, "path": None, "version": None}
    rc, out, err = run_cmd([exe, *version_args], timeout=5)
    head = (out or err).splitlines()
    return {"found": True, "path": exe, "version": head[0] if head else None, "rc": rc}


def load_probe():
    """复用 docs/规划/tools/probe_capabilities.py 的探测函数（同源，不写第二套 GPU/内存探测）。

    返回 (probe_module, error)；导入失败时 module 为 None，调用方降级处理。
    """
    probe_dir = repo_root() / "docs" / "规划" / "tools"
    if not probe_dir.exists():
        return None, f"probe 目录不存在: {probe_dir}"
    if str(probe_dir) not in sys.path:
        sys.path.insert(0, str(probe_dir))
    try:
        import probe_capabilities as probe  # type: ignore

        return probe, None
    except Exception as exc:  # 可选依赖缺失/路径异常：降级，不崩
        return None, f"probe 模块导入失败: {type(exc).__name__}: {exc}"


def gpu_free_bytes(probe_report: dict | None) -> list[dict]:
    """从同源 probe 报告中取 GPU free VRAM；报告缺失时返回空列表。"""
    if not probe_report:
        return []
    return [
        {"index": g.get("index"), "name": g.get("name"), "vram_free_bytes": g.get("vram_free_bytes")}
        for g in probe_report.get("gpus", [])
    ]


def human_size(num: int | None) -> str:
    if num is None:
        return "?"
    size = float(num)
    for unit in ("B", "KiB", "MiB", "GiB"):
        if size < 1024:
            return f"{size:.1f} {unit}"
        size /= 1024
    return f"{size:.1f} TiB"


def dir_size(path: Path) -> int:
    """递归统计目录体积；文件不可读时跳过。"""
    total = 0
    if not path.exists():
        return 0
    for entry in path.rglob("*"):
        try:
            if entry.is_file():
                total += entry.stat().st_size
        except OSError:
            continue
    return total


def vendor_status(root: Path) -> dict:
    """vendor/manifest.json 完整性体检：字段 name/version/sha256/source + 目录是否存在。"""
    manifest = root / "vendor" / "manifest.json"
    if not manifest.exists():
        return {
            "manifest": "missing",
            "entries": [],
            "note": "无 vendor/manifest.json；sync 将创建模板而不是报错",
        }
    try:
        data = json.loads(manifest.read_text(encoding="utf-8"))
    except Exception as exc:  # 清单损坏：降级标记，不崩
        return {"manifest": "invalid", "entries": [], "error": f"{type(exc).__name__}: {exc}"}

    required = ("name", "version", "sha256", "source")
    entries: list[dict] = []
    for entry in data.get("entries", []):
        name = str(entry.get("name", ""))
        entries.append(
            {
                **entry,
                "present": (root / "vendor" / name).is_dir() if name else False,
                "fields_ok": all(k in entry for k in required),
            }
        )
    return {"manifest": "present", "entries": entries}


def is_tty() -> bool:
    return sys.stdout.isatty()


def print_json(payload: dict) -> None:
    print(json.dumps(payload, ensure_ascii=False, indent=2))
