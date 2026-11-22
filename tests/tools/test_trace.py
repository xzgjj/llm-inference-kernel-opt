"""trace 子命令：按 req_id 汇总慢请求 dump（D2-10）。"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEV = REPO / "tools" / "dev.py"

RECORD = {
    "req_id": "req-abc123",
    "tenant": "t-a",
    "finish": "stop",
    "prompt_tokens": 512,
    "output_tokens": 64,
    "preemptions": 0,
    "queue_wait_ms": 12,
    "prefill_ms": 240,
    "first_token_ms": 5,
    "total_ms": 1300,
    "prompt_sha8": "0a1b2c3d",
    "prompt_redacted": True,
}


def run_trace(root: Path, *args: str) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    env["LLMOPT_ROOT"] = str(root)
    return subprocess.run([sys.executable, str(DEV), "trace", *args],
                          capture_output=True, text=True, env=env, timeout=30)


class TraceCommandTest(unittest.TestCase):
    def _write(self, root: Path, name: str, text: str) -> Path:
        dump = root / "logs" / "slow"
        dump.mkdir(parents=True, exist_ok=True)
        path = dump / name
        path.write_text(text, encoding="utf-8")
        return path

    def test_prints_four_segments_and_exits_zero(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._write(root, "req-abc123.jsonl", json.dumps(RECORD) + "\n")
            result = run_trace(root, "req-abc123")
            self.assertEqual(result.returncode, 0, result.stderr)
            for segment in ("queue_wait_ms", "prefill_ms", "first_token_ms", "total_ms"):
                self.assertIn(segment, result.stdout)
            self.assertIn("1300", result.stdout)
            self.assertIn("0a1b2c3d", result.stdout)

    def test_json_output_is_machine_readable(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._write(root, "req-abc123.jsonl", json.dumps(RECORD) + "\n")
            result = run_trace(root, "req-abc123", "--json")
            self.assertEqual(result.returncode, 0, result.stderr)
            payload = json.loads(result.stdout)
            self.assertEqual(payload["req_id"], "req-abc123")
            self.assertEqual(payload["records"][-1]["total_ms"], 1300)

    def test_missing_id_exits_two(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "logs" / "slow").mkdir(parents=True)
            result = run_trace(root, "req-missing")
            self.assertEqual(result.returncode, 2)
            self.assertIn("未找到", result.stderr)

    def test_empty_file_exits_two(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._write(root, "req-empty.jsonl", "\n\n")
            result = run_trace(root, "req-empty")
            self.assertEqual(result.returncode, 2)

    def test_invalid_limit_exits_three(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "logs" / "slow").mkdir(parents=True)
            result = run_trace(root, "req-any", "--limit", "0")
            self.assertEqual(result.returncode, 3)


if __name__ == "__main__":
    unittest.main()
