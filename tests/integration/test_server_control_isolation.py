# test_server_control_isolation.py — 控制面独立监听在数据面板被长流占满时仍可用。
from __future__ import annotations

import http.client
import json
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import server_kit as kit

CONCURRENCY = 1
DATA_POOL = CONCURRENCY + 3


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def open_stream(port: int, started: threading.Event, stop: threading.Event) -> None:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    payload = {"prompt": "control-isolation", "stream": True, "max_tokens": 32, "seed": 7}
    conn.request("POST", "/completion", json.dumps(payload),
                 {"Content-Type": "application/json"})
    response = conn.getresponse()
    check(response.status == 200, f"流式请求应 200，实际 {response.status}")
    started.set()
    try:
        while not stop.is_set():
            line = response.readline()
            if not line:
                return
    finally:
        conn.close()


def main() -> int:
    server = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as tmp:
        model = Path(tmp) / "fake.gguf"
        kit.make_gguf(model)
        control_port = kit.free_port()
        with kit.running_server(
            server, model,
            ["--ctx-tokens", "512", "--max-concurrency", str(CONCURRENCY),
             "--max-num-batched-tokens", "256", "--queue-capacity", "8",
             "--control-port", str(control_port), "--log-level", "warn"],
            {"LLMOPT_TEST_STUB_DELAY_MS": "100"},
        ) as port:
            started = [threading.Event() for _ in range(DATA_POOL)]
            stop = threading.Event()
            threads = [threading.Thread(target=open_stream, args=(port, started[i], stop),
                                        daemon=True) for i in range(DATA_POOL)]
            for thread in threads:
                thread.start()
            for index, event in enumerate(started):
                check(event.wait(timeout=5), f"数据面流 {index} 未能建立")
            # 给 handler 一点时间进入长流状态：此时数据面 DATA_POOL 个线程都已被占。
            time.sleep(0.5)

            status, body, _ = kit.get(control_port, "/healthz", timeout=2.0)
            check(status == 200, f"隔离控制面应 200，实际 {status}")
            check(json.loads(body)["status"] == "ready", f"控制面状态错误：{body!r}")

            status, body, _ = kit.get(control_port, "/metrics", timeout=2.0)
            check(status == 200, f"隔离 /metrics 应 200，实际 {status}")
            check(b"llmopt_" in body, "隔离 /metrics 应返回指标文本")

            stop.set()
            for thread in threads:
                thread.join(timeout=3)
        print("OK integration_server_control_isolation")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
