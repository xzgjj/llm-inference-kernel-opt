# test_keys_reload.py — 阶段 12 S4：静态 key 热加载轮换、失败保留旧表与 build_info 断言。
#
# 场景：
#   1. 初始 key A（id=t1）→ 200；key B → 401。
#   2. 改写文件为同 id 的新摘要（key B）→ 触发重载（POSIX: SIGHUP；Windows: 轮询 mtime）
#      → 旧 key 401、新 key 200，/metrics 出现 llmopt_api_keys_reload_failed_total。
#   3. 再写坏文件（缺 version）→ 仍保持新表（B 200 / A 401），失败指标 +1。
#   4. /metrics 的 llmopt_build_info version 与 `--version` 输出一致（S6 代码面）。
#
# 平台分支：Windows 无 SIGHUP，真跑 --api-keys-reload-seconds 1 轮询分支；
# POSIX 走 SIGHUP 分支（WSL Ubuntu-26.04 stub 二进制实测，见阶段 12 S4 记录）。
from __future__ import annotations

import hashlib
import http.client
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import server_kit as kit  # noqa: E402

KEY_A = "lk_" + "a" * 32
KEY_B = "lk_" + "b" * 32

QUOTA = {"max_concurrency": 1, "queue_capacity": 1}


def sha256(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()


def write_keys(path: Path, entries: list[dict]) -> None:
    path.write_text(json.dumps({"version": 1, "keys": entries}), encoding="utf-8")
    os.chmod(path, 0o600)


def bearer_post(port: int, key: str, payload: dict) -> tuple[int, dict]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    headers = {"Content-Type": "application/json"}
    if key:
        headers["Authorization"] = f"Bearer {key}"
    try:
        conn.request("POST", "/v1/completions", json.dumps(payload), headers)
        response = conn.getresponse()
        body = json.loads(response.read())
        return response.status, body
    finally:
        conn.close()


def reload_trigger(proc: subprocess.Popen[bytes]) -> None:
    """触发一次重读：POSIX 发 SIGHUP；Windows 走轮询分支，靠 sleep 等下一轮 mtime 检查。"""
    if os.name == "nt":
        # 轮询周期 1 s、线程 500 ms 节拍；等 3 s 覆盖至少两轮 stat/mtime 比对。
        time.sleep(3.0)
    else:
        # POSIX SIGHUP：handler 只置原子标志，reload 线程 500 ms 节拍内消费；
        # 1.5 s 覆盖一次完整节拍并留出重载 IO 余量。
        os.kill(proc.pid, signal.SIGHUP)
        time.sleep(1.5)


def read_metrics(port: int) -> str:
    status, body, _ = kit.get(port, "/metrics")
    assert status == 200, f"/metrics 应 200，实际 {status}"
    return body.decode()


def metric_value(metrics_text: str, series: str) -> float:
    samples, _, _, _ = kit.parse_metrics(metrics_text)
    return samples[series]


def main() -> int:
    server = Path(sys.argv[1])
    tmp = Path(tempfile.mkdtemp(prefix="llmopt-keys-reload-"))
    keep = os.environ.get("LLMOPT_KEEP_TMP") == "1"
    try:
        model = tmp / "reload.gguf"
        kit.make_gguf(model)
        keys = tmp / "keys.json"
        write_keys(keys, [{"id": "t1", "sha256": sha256(KEY_A), "quota": QUOTA}])

        args = ["--api-keys-file", str(keys), "--max-concurrency", "4",
                "--queue-capacity", "8", "--block-size", "4",
                "--max-num-batched-tokens", "512", "--chunk-size", "32"]
        if os.name == "nt":
            # Windows 无 SIGHUP：显式开启 1 s 轮询，本机必须真跑这条分支。
            args += ["--api-keys-reload-seconds", "1"]

        port = kit.free_port()
        env = dict(os.environ)
        env["LLMOPT_ALLOW_STUB"] = "1"
        log_out = open(tmp / "server.out.log", "wb")
        log_err = open(tmp / "server.err.log", "wb")
        cmd = [str(server), "--model", str(model), "--host", "127.0.0.1", "--port", str(port),
               "--backend", "stub", "--mode", "off", "--log-level", "info"] + args
        proc = subprocess.Popen(cmd, stdout=log_out, stderr=log_err, env=env)
        try:
            kit.wait_ready(port, proc)
            payload = {"prompt": "hello", "max_tokens": 2}

            # 1) 初始表：A 200，B 401。
            status, body = bearer_post(port, KEY_A, payload)
            assert status == 200, (status, body)
            status, body = bearer_post(port, KEY_B, payload)
            assert status == 401, (status, body)

            # 2) 轮换为同 id 的新摘要 B，触发重载。
            write_keys(keys, [{"id": "t1", "sha256": sha256(KEY_B), "quota": QUOTA}])
            reload_trigger(proc)

            status, body = bearer_post(port, KEY_A, payload)
            assert status == 401, f"旧 key 应已失效，实际 {status} {body}"
            status, body = bearer_post(port, KEY_B, payload)
            assert status == 200, f"新 key 应命中，实际 {status} {body}"

            metrics_text = read_metrics(port)
            assert "llmopt_api_keys_reload_failed_total" in metrics_text, \
                "缺少 reload 失败指标序列"

            # 3) 写坏文件（缺 version）：保留新表，失败指标 +1。
            before_failed = metric_value(metrics_text, "llmopt_api_keys_reload_failed_total")
            keys.write_text(json.dumps({"keys": []}), encoding="utf-8")
            reload_trigger(proc)

            status, body = bearer_post(port, KEY_A, payload)
            assert status == 401, f"坏文件后旧 key 仍应 401，实际 {status}"
            status, body = bearer_post(port, KEY_B, payload)
            assert status == 200, f"坏文件后新 key 应保持 200，实际 {status} {body}"

            metrics_text = read_metrics(port)
            after_failed = metric_value(metrics_text, "llmopt_api_keys_reload_failed_total")
            assert after_failed == before_failed + 1, \
                f"失败指标应 +1：{before_failed} → {after_failed}"

            # 4) S6：build_info 存在，且 version 与 --version 输出一致。
            version_proc = subprocess.run([str(server), "--version"], capture_output=True,
                                          text=True, timeout=10)
            assert version_proc.returncode == 0, version_proc.stderr
            version_line = version_proc.stdout.strip().splitlines()[0]
            assert version_line.startswith("llmopt "), version_line
            version = version_line.split()[1]
            assert f'llmopt_build_info{{version="{version}"' in metrics_text, \
                f"build_info 缺少 version={version}：{metrics_text.splitlines()[-1]}"
            print(f"keys reload ok (version={version}, failed={after_failed}, "
                  f"platform={'windows-poll' if os.name == 'nt' else 'posix-sighup'})")
        except AssertionError:
            sys.stdout.write("---- server.out.log ----\n")
            sys.stdout.write((tmp / "server.out.log").read_text(encoding="utf-8", errors="replace"))
            sys.stdout.write("---- server.err.log ----\n")
            sys.stdout.write((tmp / "server.err.log").read_text(encoding="utf-8", errors="replace"))
            raise
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
            log_out.close()
            log_err.close()
    finally:
        if not keep:
            shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
