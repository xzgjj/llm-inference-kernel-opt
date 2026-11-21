"""容量档位判据：只解析服务自己的 estimate 行，不补算、不伪造。"""

from __future__ import annotations

import csv
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from scripts.bench import capacity_check


SAMPLE = """config: model= host=127.0.0.1 port=8080 ctx=1024 max_concurrency=4
capacity-profile: path=/tmp/27b.json name=qwen3-27b-dense-q4 bytes=17609365914 layers=64
estimate: model=16792MiB kv=1024MiB required=17816MiB free=5200MiB safety=0.8 decision=BLOCKED
estimate-split: model_vram=4198MiB kv_vram=256MiB host=13362MiB
gate: 已卸载部分超过可用显存，拒绝启动
capacity-only: allow_start=0
"""


class ParseOutputTest(unittest.TestCase):
    def test_extracts_measurements_without_estimating(self) -> None:
        row = capacity_check.parse_output(SAMPLE)
        self.assertEqual(row["profile"], "qwen3-27b-dense-q4")
        self.assertEqual(row["model_mib"], 16792)
        self.assertEqual(row["kv_mib"], 1024)
        self.assertEqual(row["required_mib"], 17816)
        self.assertEqual(row["free_mib"], 5200)
        self.assertEqual(row["model_vram_mib"], 4198)
        self.assertEqual(row["kv_vram_mib"], 256)
        self.assertEqual(row["host_mib"], 13362)
        self.assertEqual(row["decision"], "BLOCKED")
        self.assertEqual(row["allow_start"], 0)
        self.assertIn("超过可用显存", row["reason"])

    def test_missing_lines_stay_blank(self) -> None:
        row = capacity_check.parse_output("config: model=\n")
        self.assertEqual(row["decision"], "")
        self.assertEqual(row["allow_start"], "")
        self.assertEqual(row["model_mib"], "")

    def test_half_line_and_garbage_do_not_crash(self) -> None:
        row = capacity_check.parse_output("estimate: model=16792MiB kv=1024MiB\n"
                                          "garbage 无数字 [x]\n")
        self.assertEqual(row["decision"], "")
        self.assertEqual(row["model_mib"], "")
        self.assertEqual(row["required_mib"], "")


class RunOneTest(unittest.TestCase):
    def test_command_carries_capacity_only_and_profile(self) -> None:
        completed = mock.Mock(stdout=SAMPLE, stderr="", returncode=3)
        with mock.patch.object(capacity_check.subprocess, "run", return_value=completed) as runner:
            row = capacity_check.run_one(Path("server"), Path("p.json"), 16, 1024, 4, 1024, "q8_0")
        command = runner.call_args.args[0]
        self.assertIn("--capacity-only", command)
        self.assertIn("--model-profile", command)
        self.assertIn("--n-gpu-layers", command)
        self.assertIn("--disable-prefix-cache", command)
        self.assertEqual(row["exit_code"], 3)
        self.assertEqual(row["n_gpu_layers"], 16)
        self.assertEqual(row["kv_cache_type"], "q8_0")

    def test_timeout_expired_keeps_failed_row(self) -> None:
        exc = subprocess.TimeoutExpired(["server"], 120, output="", stderr="")
        with mock.patch.object(capacity_check.subprocess, "run", side_effect=exc):
            row = capacity_check.run_one(Path("server"), Path("p.json"), 16, 1024, 4, 1024, "q8_0")
        self.assertEqual(row["exit_code"], 124)
        self.assertIn("超时", row["reason"])
        self.assertEqual(row["decision"], "")

    def test_oserror_keeps_failed_row(self) -> None:
        with mock.patch.object(capacity_check.subprocess, "run", side_effect=OSError("no such file")):
            row = capacity_check.run_one(Path("server"), Path("p.json"), 16, 1024, 4, 1024, "q8_0")
        self.assertEqual(row["exit_code"], 1)
        self.assertIn("启动失败", row["reason"])


class MainTest(unittest.TestCase):
    def test_writes_csv_rows(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp) / "p.json"
            profile.write_text(json.dumps({"name": "x", "file_bytes": 1}), encoding="utf-8")
            server = Path(tmp) / "server"
            server.write_text("", encoding="utf-8")
            completed = mock.Mock(stdout=SAMPLE, stderr="", returncode=3)
            with mock.patch.object(capacity_check.subprocess, "run", return_value=completed):
                rc = capacity_check.main([
                    "--server", str(server), "--profile", str(profile),
                    "--ngl", "8,16", "--out", str(Path(tmp) / "out.csv"),
                ])
            with (Path(tmp) / "out.csv").open(encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
        self.assertEqual(rc, 0)
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0]["n_gpu_layers"], "8")
        self.assertEqual(rows[1]["n_gpu_layers"], "16")

    def test_failed_row_does_not_lose_earlier_rows(self) -> None:
        """超时档位落失败占位行，已跑完的档位仍在 CSV 里。"""
        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp) / "p.json"
            profile.write_text(json.dumps({"name": "x", "file_bytes": 1}), encoding="utf-8")
            server = Path(tmp) / "server"
            server.write_text("", encoding="utf-8")
            completed = mock.Mock(stdout=SAMPLE, stderr="", returncode=3)
            exc = subprocess.TimeoutExpired(["server"], 120, output="", stderr="")
            with mock.patch.object(capacity_check.subprocess, "run", side_effect=[completed, exc]):
                rc = capacity_check.main([
                    "--server", str(server), "--profile", str(profile),
                    "--ngl", "8,16", "--out", str(Path(tmp) / "out.csv"),
                ])
            with (Path(tmp) / "out.csv").open(encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
        self.assertEqual(rc, 0)
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0]["exit_code"], "3")
        self.assertEqual(rows[1]["exit_code"], "124")


if __name__ == "__main__":
    unittest.main()
