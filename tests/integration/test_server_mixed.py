# test_server_mixed.py — 4-G6：长短 1:1 同轮混合，分组入表、长请求不饿死。
from __future__ import annotations

import csv
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import server_kit as kit

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "scripts" / "bench" / "matrix_runner.py"


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    server = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as tmp:
        model = Path(tmp) / "fake.gguf"
        kit.make_gguf(model)
        out = Path(tmp) / "mixed"
        env = dict(os.environ)
        env["LLMOPT_ALLOW_STUB"] = "1"
        result = subprocess.run(
            [sys.executable, str(RUNNER), "--smoke", "--mixed", "--server", str(server),
             "--model", str(model), "--out", str(out), "--run-id", "mixed-test"],
            capture_output=True, text=True, timeout=180, env=env,
        )
        check(result.returncode == 0, f"混合矩阵失败\n{result.stdout}\n{result.stderr}")
        with (out / "matrix_report.csv").open(encoding="utf-8") as handle:
            rows = list(csv.DictReader(handle))
        check(len(rows) == 4, f"应 4 行分组结果，实际 {len(rows)}")
        for row in rows:
            check(row["status"] == "ok", f"混合档不应失败：{row}")
            check(int(row["n_requests"]) == 3, "短/长各 3 条都应有结果")
            check(float(row["ttft_p50_ms"]) > 0, "TTFT 应分组可算")
        tiers = {row["prompt_tier"] for row in rows}
        check(tiers == {"short", "long"}, f"分组列应保留两档，实际 {tiers}")

        records = [json.loads(line) for line in
                   (out / "requests.jsonl").read_text(encoding="utf-8").splitlines() if line]
        for concurrency in (1, 2):
            long_rows = [row for row in records if row["concurrency"] == concurrency
                         and row["tier"] == "long" and not row["dropped"]]
            check(len(long_rows) == 3, "长请求不能被短请求挤掉")
            check(all(row["status"] == "ok" for row in long_rows), "长请求应全部完成")
        print("OK integration_server_mixed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
