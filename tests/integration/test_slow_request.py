"""test_slow_request.py — D2-10：慢请求 dump 生成、脱敏与 dev.py trace 回放。"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import server_kit as kit

REPO = Path(__file__).resolve().parents[2]


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    server = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        model = root / "fake.gguf"
        kit.make_gguf(model)
        dump_dir = root / "logs" / "slow"
        with kit.running_server(server, model,
                               ["--ctx-tokens", "512", "--max-concurrency", "1",
                                "--max-num-batched-tokens", "256", "--log-level", "warn",
                                "--slow-request-ms", "50",
                                "--slow-dump-dir", str(dump_dir)],
                               env_extra={"LLMOPT_TEST_STUB_DELAY_MS": "20"}) as port:
            prompt = "ZZQX 敏感原文 J-77b"
            status, body, _ = kit.post(port, "/completion",
                                       {"prompt": prompt, "max_tokens": 6, "seed": 7})
            check(status == 200, f"请求应 200，实际 {status}")
            payload = json.loads(body)
            req_id = payload.get("id") or payload.get("req_id")
            check(bool(req_id), "响应必须带 request id，否则无法按 id 定位 dump")

        files = sorted(dump_dir.glob("*.jsonl")) if dump_dir.is_dir() else []
        check(len(files) == 1, f"应恰好生成 1 份慢请求 dump，实际 {len(files)}")
        record_text = files[0].read_text(encoding="utf-8")
        record = json.loads(record_text.splitlines()[-1])
        for key in ("queue_wait_ms", "prefill_ms", "first_token_ms", "total_ms"):
            check(key in record, f"dump 缺少四段计时字段 {key}")
        check(record["total_ms"] >= 50, "total_ms 应不低于阈值")
        check(record["prompt_redacted"] is True, "dump 必须标记脱敏")
        check(len(record["prompt_sha8"]) == 8, "prompt 只保留 8 位指纹")
        # 脱敏硬约束：dump 里不能出现 prompt 原文的任何片段。
        for token in ("ZZQX", "敏感原文", "J-77b"):
            check(token not in record_text, f"dump 泄漏 prompt 片段：{token}")
        check(record["req_id"] == files[0].stem, "文件名必须等于 req_id")

        env = dict(os.environ)
        env["LLMOPT_ROOT"] = str(root)
        proc = subprocess.run([sys.executable, str(REPO / "tools" / "dev.py"), "trace",
                               record["req_id"], "--dir", str(dump_dir)],
                              capture_output=True, text=True, env=env, timeout=30)
        check(proc.returncode == 0, f"dev.py trace 应退出 0，实际 {proc.returncode}: {proc.stderr}")
        for segment in ("queue_wait_ms", "prefill_ms", "first_token_ms", "total_ms"):
            check(segment in proc.stdout, f"trace 输出缺少 {segment}")
    print("OK integration_slow_request")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
