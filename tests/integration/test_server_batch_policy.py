# test_server_batch_policy.py — 黑盒对照：static 批必须整批跑空才放下一批，
# continuous 批在短请求完成后立刻补位。回归 P1-3 的"槽位复用"没有基线可测。
#
# 判定不用总墙钟（并行负载下会被放大），而是看"短请求让出的槽位什么时候被新请求用上"：
# 先占满 2 个槽（一条长、一条短），短的回包后立刻发第三条长请求，量它的首帧延迟。
# continuous 应立即开始；static 必须等第一条长请求跑完。
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

CONCURRENCY = 2
STEP_MS = 250
LONG_TOKENS = 16
SHORT_TOKENS = 4


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def stream_ttft(port: int, prompt: str, max_tokens: int) -> float:
    """发一条流式请求，返回从发出到第一个 SSE 帧的时间；读完整个响应再关闭。"""
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=120)
    body = json.dumps({"prompt": prompt, "stream": True, "max_tokens": max_tokens,
                       "seed": 11})
    began = time.monotonic()
    connection.request("POST", "/completion", body, {"Content-Type": "application/json"})
    response = connection.getresponse()
    check(response.status == 200, f"流式请求应 200，实际 {response.status}")
    ttft: float | None = None
    while True:
        line = response.readline()
        if not line:
            break
        if ttft is None and line.startswith(b"data:"):
            ttft = time.monotonic() - began
    connection.close()
    check(ttft is not None, "流式响应没有 data 帧")
    return float(ttft)


def measure(policy: str, server: Path) -> float:
    with tempfile.TemporaryDirectory() as tmp:
        model = Path(tmp) / "fake.gguf"
        kit.make_gguf(model)
        with kit.running_server(
                server, model,
                ["--ctx-tokens", "512", "--max-concurrency", str(CONCURRENCY),
                 "--queue-capacity", "8", "--chunk-size", "32",
                 "--max-num-batched-tokens", "64", "--batch-policy", policy,
                 "--log-level", "warn"],
                {"LLMOPT_TEST_STUB_DELAY_MS": str(STEP_MS)}) as port:
            long_result: dict[str, object] = {}
            third_ttft: dict[str, float] = {}

            def fire_long_a() -> None:
                status, _, _ = kit.post(
                    port, "/completion",
                    {"prompt": "long-a", "max_tokens": LONG_TOKENS, "seed": 1},
                    timeout=120.0)
                long_result["status"] = status

            def fire_short_b() -> None:
                kit.post(port, "/completion",
                         {"prompt": "short-b", "max_tokens": SHORT_TOKENS, "seed": 2},
                         timeout=120.0)

            def fire_long_c() -> None:
                third_ttft["value"] = stream_ttft(port, "long-c", LONG_TOKENS)

            long_thread = threading.Thread(target=fire_long_a)
            long_thread.start()
            time.sleep(0.2)  # 让第一条长请求先占住一个槽位
            # B、C 几乎同时到达：continuous 下 C 等 B（短）让位，static 下 C 等 A（长）跑完整批。
            short_thread = threading.Thread(target=fire_short_b)
            third_thread = threading.Thread(target=fire_long_c)
            short_thread.start()
            third_thread.start()

            for thread in (short_thread, third_thread, long_thread):
                thread.join(timeout=120)
                check(not thread.is_alive(), "请求应在超时前完成")
            check(long_result.get("status") == 200,
                  f"长请求应 200，实际 {long_result.get('status')}")
            check("value" in third_ttft, "第三条长请求没有首帧时间")
            return float(third_ttft["value"])


def main() -> int:
    server = Path(sys.argv[1])
    continuous = measure("continuous", server)
    static = measure("static", server)
    check(static > continuous + 1.0,
          f"static 的补位延迟应明显更长：continuous={continuous:.2f}s static={static:.2f}s")
    print(f"OK integration_server_batch_policy ttft continuous={continuous:.2f}s "
          f"static={static:.2f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
