"""从 D2-03 四组矩阵 CSV 生成 README 容量表块（不手写数字）。

输入：evidence/phase13/*/matrix_report.csv（列见 matrix.REPORT_FIELDS）。
输出：Markdown 表，按组 → P 排序；`deviation_flag=insufficient` 的行照抄并标 †，
表示样本不足、不能拿来做结论。任何数字都来自 CSV，脚本不做四舍五入以外的加工。
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

GROUPS = ("perf-static", "perf-mixed", "perf-prefix-on", "perf-prefix-off")


def read_group(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def _int(value: str) -> str:
    try:
        return str(int(float(value)))
    except (TypeError, ValueError):
        return "-"


def _ms(value: str) -> str:
    if not value:
        return "-"
    return f"{float(value):.1f}"


def _tok(value: str) -> str:
    if not value:
        return "-"
    return f"{float(value):.1f}"


def render(evidence: Path) -> str:
    lines = [
        "| 组 | P | TTFT p50 (ms) | ITL p99 (ms) | 总 token 吞吐 (tok/s) | VRAM 峰值 (MiB) | 备注 |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ]
    for group in GROUPS:
        path = evidence / group / "matrix_report.csv"
        if not path.is_file():
            lines.append(f"| {group} | - | - | - | - | - | 缺 CSV |")
            continue
        for row in read_group(path):
            note = "" if row.get("deviation_flag") == "ok" else f"{row.get('deviation_flag')}†"
            lines.append(
                "| {group} | {p} | {ttft} | {itl} | {tok} | {vram} | {note} |".format(
                    group=group,
                    p=_int(row.get("concurrency_p", "")),
                    ttft=_ms(row.get("ttft_p50_ms", "")),
                    itl=_ms(row.get("itl_p99_ms", "")),
                    tok=_tok(row.get("total_tok_s", "")),
                    vram=_int(row.get("vram_peak_mib", "")),
                    note=note,
                )
            )
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="生成容量表（D2-03 → README）")
    parser.add_argument("--evidence", default="evidence/phase13", type=Path)
    parser.add_argument("--out", type=Path, help="写入文件；缺省打印到 stdout")
    args = parser.parse_args(argv)
    if not args.evidence.is_dir():
        print(f"证据目录不存在: {args.evidence}", file=sys.stderr)
        return 3
    text = render(args.evidence)
    if args.out:
        args.out.write_text(text + "\n", encoding="utf-8")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
