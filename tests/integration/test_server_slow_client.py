# test_server_slow_client.py — 4-G0-2：慢读客户端不阻塞快请求，溢出计数可见。
from __future__ import annotations

import csv
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import server_kit as kit

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "bench" / "slow_client.py"


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    server = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as tmp:
        model = Path(tmp) / "fake.gguf"
        kit.make_gguf(model)
        out = Path(tmp) / "backpressure.csv"
        with kit.running_server(
            server, model,
            ["--ctx-tokens", "512", "--max-concurrency", "2",
             "--max-num-batched-tokens", "256", "--log-level", "warn"],
            {"LLMOPT_TEST_STUB_DELAY_MS": "5"},
        ) as port:
            result = subprocess.run(
                [sys.executable, str(SCRIPT), "--base", f"http://127.0.0.1:{port}",
                 "--out", str(out), "--tokens", "64", "--frames", "2", "--delay", "0.3"],
                capture_output=True, text=True, timeout=90,
            )
        check(result.returncode == 0, f"慢读探测失败\n{result.stdout}\n{result.stderr}")
        with out.open(encoding="utf-8") as handle:
            rows = list(csv.DictReader(handle))
        check(len(rows) == 1, "应输出一行 backpressure 记录")
        row = rows[0]
        check(int(row["fast_frames"]) > 0, "快客户端必须完整读到 token")
        check(int(row["slow_frames"]) >= 1, "慢客户端至少读 1 帧")
        check(float(row["fast_itl_p99_ms"]) > 0, "快客户端 ITL p99 应为正")
        check(row["overflow_before"] != "" and row["overflow_after"] != "",
              "溢出计数必须可读")
        check(float(row["overflow_after"]) >= float(row["overflow_before"]), "溢出计数不应回退")
        print("OK integration_server_slow_client")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
