# test_drain.py — 6-G3：SIGTERM 后 /readyz 立即 503、新请求 503、在飞请求拿到终态、退出码 0。
# Windows 的 TerminateProcess 没有 SIGTERM 语义，本机跳过；CI Linux 真跑。
from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import server_kit as kit  # noqa: E402


def main() -> int:
    if os.name == "nt":
        print("drain test skipped on Windows (no SIGTERM semantics)")
        return 0
    server = Path(sys.argv[1])
    workdir = Path(tempfile.mkdtemp(prefix="drain-"))
    model = workdir / "drain.gguf"
    kit.make_gguf(model)
    port = kit.free_port()
    env = dict(os.environ)
    env["LLMOPT_ALLOW_STUB"] = "1"
    env["LLMOPT_TEST_STUB_DELAY_MS"] = "40"
    cmd = [str(server), "--model", str(model), "--host", "127.0.0.1", "--port", str(port),
           "--backend", "stub", "--mode", "off", "--drain-deadline-seconds", "30",
           "--max-concurrency", "8"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                            cwd=str(workdir))
    results: list[tuple[int, bytes]] = []
    lock = threading.Lock()

    def worker(index: int) -> None:
        try:
            status, body, _ = kit.post(port, "/v1/completions",
                                       {"prompt": f"inflight-{index}", "stream": True,
                                        "max_tokens": 16, "seed": index}, timeout=60)
            with lock:
                results.append((status, body))
        except OSError as exc:  # 连接被强制断开也算可观察终态
            with lock:
                results.append((-1, str(exc).encode()))

    try:
        kit.wait_ready(port, proc)
        threads = [threading.Thread(target=worker, args=(i,)) for i in range(8)]
        for thread in threads:
            thread.start()
        time.sleep(0.4)
        proc.send_signal(signal.SIGTERM)
        deadline = time.time() + 0.1
        last_status = 200
        while time.time() < deadline:
            status, body, _ = kit.get(port, "/readyz", timeout=2)
            last_status = status
            if status == 503:
                break
            time.sleep(0.02)
        assert last_status == 503, f"/readyz 未在 100ms 内转 503，实际 {last_status}"
        status, body, _ = kit.post(port, "/v1/completions", {"prompt": "new", "max_tokens": 2})
        assert status == 503, f"排空中新请求应 503，实际 {status}"
        assert json.loads(body)["error"]["code"] == "SERVICE_UNAVAILABLE", body
        for thread in threads:
            thread.join(timeout=60)
            assert not thread.is_alive(), "在飞请求未在排空期限内收尾"
        for status, body in results:
            assert status in (200, -1), status
            if status == 200:
                assert b"data: [DONE]" in body, body[-400:]
        rc = proc.wait(timeout=40)
        assert rc == 0, f"排空后退出码应为 0，实际 {rc}"
        print(f"drain ok: inflight={len(results)} rc={rc}")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        out, _ = proc.communicate()
        text = (out or b"").decode("utf-8", errors="replace")
        assert "draining" in text, text[-4000:]
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
