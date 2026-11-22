"""trace — 按 req_id 还原一条请求的生命周期（D2-10）。

数据来源：服务端 `--slow-request-ms` 落下的 `<dir>/<req_id>.jsonl`（默认 `logs/slow`）。
只读不写；找不到文件或文件为空一律退出 2（环境不可用），参数非法退出 3。
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

from dev_commands import common

NAME = "trace"
HELP = "按 req_id 汇总慢请求四段耗时与生命周期计数（读 logs/slow/*.jsonl）"

SEGMENTS = ("queue_wait_ms", "prefill_ms", "first_token_ms", "total_ms")
COUNTERS = ("prompt_tokens", "output_tokens", "preemptions")


def add_arguments(parser) -> None:  # noqa: ANN001 - argparse 子解析器
    parser.add_argument("req_id", help="请求 id（服务端 req-… 或客户端自带的 id）")
    parser.add_argument("--dir", default="logs/slow",
                        help="dump 目录，默认 logs/slow（相对仓库根）")
    parser.add_argument("--limit", type=int, default=1,
                        help="同一 id 有多条记录时打印最后 N 条，默认 1")


def _records(path: Path) -> list[dict]:
    records: list[dict] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            item = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(item, dict):
            records.append(item)
    return records


def run(args) -> int:  # noqa: ANN001 - argparse Namespace
    if args.limit < 1:
        print("参数错误: --limit 必须 >= 1", file=sys.stderr)
        return common.EXIT_ARG_ERROR
    directory = Path(args.dir)
    if not directory.is_absolute():
        directory = common.repo_root() / directory
    path = directory / f"{args.req_id}.jsonl"
    if not path.is_file():
        print(f"未找到 {path}；确认服务端开启了 --slow-request-ms 且该请求超过阈值",
              file=sys.stderr)
        return common.EXIT_ENV_UNAVAILABLE
    records = _records(path)
    if not records:
        print(f"{path} 里没有可解析的记录", file=sys.stderr)
        return common.EXIT_ENV_UNAVAILABLE

    selected = records[-args.limit:]
    if args.json:
        common.print_json({"req_id": args.req_id, "records": selected})
        return common.EXIT_OK

    latest = selected[-1]
    print(f"req_id: {latest.get('req_id', args.req_id)}")
    print(f"tenant: {latest.get('tenant', 'default')}")
    print(f"finish: {latest.get('finish', 'unknown')}")
    print("四段耗时（毫秒）:")
    for segment in SEGMENTS:
        print(f"  {segment:16s} {latest.get(segment, 0)}")
    print("计数:")
    for counter in COUNTERS:
        print(f"  {counter:16s} {latest.get(counter, 0)}")
    print(f"  prompt_sha8      {latest.get('prompt_sha8', '')}")
    if len(records) > 1:
        print(f"备注: 该 id 共 {len(records)} 条记录，已打印最后 {len(selected)} 条")
    return common.EXIT_OK
