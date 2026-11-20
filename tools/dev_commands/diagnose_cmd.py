"""diagnose — 日志归类诊断。

扫描 build/logs/ 与 logs/，按规则提取：OOM/显存不足、断言失败、段错误、超时、
429、客户端断连、调度单步耗时超阈值、KV 分配/释放计数不匹配；
输出"现象 → 可能原因 → 下一步命令"三段式，附最近一次运行配置快照。
"""

from __future__ import annotations

import json
import re
import time
from pathlib import Path

from dev_commands import common

NAME = "diagnose"
HELP = "扫描日志归类故障，三段式输出现象/原因/下一步命令"

SLOW_STEP_THRESHOLD_MS = 50
SCAN_DIRS = ("build/logs", "logs")
SCAN_SUFFIX = (".log", ".txt")

RULES = [
    {
        "id": "oom",
        "patterns": [r"out of memory", r"\bOOM\b", r"显存不足", r"CUDA out of memory",
                     r"failed to allocate"],
        "title": "OOM / 显存或内存不足",
        "cause": "并发路数、上下文长度或量化档位超出本机预算；对照 build/env.json 的 free VRAM",
        "next": "tools/dev.py env --json",
    },
    {
        "id": "assert",
        "patterns": [r"assertion failed", r"AssertionError", r"断言失败"],
        "title": "断言失败",
        "cause": "调度状态机不变量被破坏（seq/block 归属或计数不一致），优先查最近改动的路径",
        "next": "tools/dev.py test --label fast",
    },
    {
        "id": "segv",
        "patterns": [r"segmentation fault", r"\bSIGSEGV\b", r"段错误"],
        "title": "段错误",
        "cause": "空指针/越界访问；留 core dump 后用 debug 命令全量回溯",
        "next": "tools/dev.py debug --bin <bin> --core <core>",
    },
    {
        "id": "timeout",
        "patterns": [r"timed out", r"\btimeout\b", r"超时"],
        "title": "超时",
        "cause": "单步/请求超时；可能是后端卡顿、排队过长或上游慢",
        "next": "tools/dev.py diagnose --since 1h",
    },
    {
        "id": "http_429",
        "patterns": [r"\b429\b", r"rate limit", r"限流"],
        "title": "HTTP 429 / 限流",
        "cause": "准入水位或并发上限触发限流；检查活跃请求数与 KV 余量",
        "next": "tools/dev.py env --json",
    },
    {
        "id": "disconnect",
        "patterns": [r"client disconnected", r"connection reset", r"断连",
                     r"Broken pipe", r"\bEPIPE\b"],
        "title": "客户端断连",
        "cause": "连接断开后取消信号未走 Mailbox，块/slot 可能未回收；查 KV 计数",
        "next": "tools/dev.py diagnose --since 1h",
    },
    {
        "id": "slow_step",
        "patterns": [r"step (?:took|cost)\s+(\d+)\s*ms", r"调度单步耗时\s*(\d+)\s*ms"],
        "title": "调度单步耗时超阈值",
        "cause": "单步临界区过长或同步阻塞；阈值 "
                 f"{SLOW_STEP_THRESHOLD_MS}ms",
        "next": "tools/dev.py test --bench",
    },
    {
        "id": "kv_mismatch",
        "patterns": [r"KV (?:alloc|free) count mismatch", r"alloc/free count mismatch",
                     r"块.*释放计数不匹配", r"block.*leak", r"KV 分配/释放计数不匹配"],
        "title": "KV 分配/释放计数不匹配",
        "cause": "块表与后端 cell 账本不一致；对照阶段 2 对账测试项",
        "next": "tools/dev.py test --label fast",
    },
]


def _parse_since(text: str) -> float:
    """'30m' / '1h' / '2d' → 秒数；非法值回落 1h。"""
    match = re.fullmatch(r"(\d+)\s*([mhd])", text.strip())
    if not match:
        return 3600
    value, unit = int(match.group(1)), match.group(2)
    return value * {"m": 60, "h": 3600, "d": 86400}[unit]


def _iter_log_files(root: Path, cutoff: float):
    for rel in SCAN_DIRS:
        directory = root / rel
        if not directory.exists():
            continue
        for path in directory.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in SCAN_SUFFIX:
                continue
            try:
                if path.stat().st_mtime >= cutoff:
                    yield path
            except OSError:
                continue


def _match_rule(rule: dict, line: str) -> dict | None:
    for pattern in rule["patterns"]:
        match = re.search(pattern, line, re.IGNORECASE)
        if match:
            detail = None
            if rule["id"] == "slow_step" and match.lastindex:
                ms = int(match.group(1))
                if ms < SLOW_STEP_THRESHOLD_MS:
                    return None
                detail = f"{ms} ms"
            return {"snippet": line.strip()[:200], "detail": detail}
    return None


def _load_snapshot(root: Path) -> dict | None:
    env_path = root / "build" / "env.json"
    if not env_path.exists():
        return None
    try:
        data = json.loads(env_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return None
    return {
        "timestamp": data.get("timestamp"),
        "toolchain": data.get("toolchain"),
        "gpus": common.gpu_free_bytes(data.get("probe")),
        "sync": data.get("sync"),
    }


def add_arguments(parser) -> None:
    parser.add_argument("--since", default="1h", help="扫描最近多久的日志，如 30m/1h/2d")


def run(args) -> int:
    root = common.repo_root()
    cutoff = time.time() - _parse_since(args.since)
    findings: list[dict] = []

    for log_file in _iter_log_files(root, cutoff):
        try:
            lines = log_file.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for lineno, line in enumerate(lines, 1):
            for rule in RULES:
                hit = _match_rule(rule, line)
                if hit:
                    findings.append({
                        "rule": rule["id"],
                        "title": rule["title"],
                        "file": str(log_file.relative_to(root)),
                        "line": lineno,
                        **hit,
                        "possible_cause": rule["cause"],
                        "next_command": rule["next"],
                    })

    snapshot = _load_snapshot(root)
    payload = {"since": args.since, "findings": findings, "snapshot": snapshot}

    if getattr(args, "json", False):
        common.print_json(payload)
    else:
        if not findings:
            print(f"最近 {args.since} 未匹配到已知故障规则")
        for item in findings:
            print(f"现象: [{item['rule']}] {item['title']}")
            print(f"  证据: {item['file']}:{item['line']} {item['snippet']}"
                  + (f"（{item['detail']}）" if item["detail"] else ""))
            print(f"  可能原因: {item['possible_cause']}")
            print(f"  下一步: {item['next_command']}")
            print()
        if snapshot:
            print("最近运行配置快照:")
            print(json.dumps(snapshot, ensure_ascii=False, indent=2))
    return common.EXIT_OK
