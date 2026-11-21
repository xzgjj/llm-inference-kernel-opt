# test_server_queue_capacity.py — 队列容量 >3 时 429 必须在入口可达。
# 回归：HTTP 线程数曾是 concurrency+3，capacity=并发+队列 永远到不了，超额请求堆在
# cpp-httplib 的无界 job 队列里——客户端只会超时，QUEUE_FULL 计数恒为 0。
from __future__ import annotations

import json
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import server_kit as kit

CONCURRENCY = 1
QUEUE = 16
CAPACITY = CONCURRENCY + QUEUE
REQUEST_COUNT = CAPACITY + 3


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    server = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as tmp:
        model = Path(tmp) / "fake.gguf"
        kit.make_gguf(model)
        with kit.running_server(
                server, model,
                ["--ctx-tokens", "512", "--max-concurrency", str(CONCURRENCY),
                 "--queue-capacity", str(QUEUE), "--chunk-size", "32",
                 "--max-num-batched-tokens", "32", "--log-level", "warn"],
                {"LLMOPT_TEST_STUB_DELAY_MS": "600"}) as port:
            start = threading.Barrier(REQUEST_COUNT)
            results: list[tuple[int, bytes, dict]] = []
            lock = threading.Lock()

            def fire(index: int) -> None:
                start.wait()
                try:
                    outcome = kit.post(port, "/completion",
                                       {"prompt": f"capacity-{index}", "stream": True,
                                        "max_tokens": 2, "seed": index})
                except OSError as exc:
                    outcome = (0, str(exc).encode(), {})
                with lock:
                    results.append(outcome)

            threads = [threading.Thread(target=fire, args=(index,))
                       for index in range(REQUEST_COUNT)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(timeout=60)
            check(len(results) == REQUEST_COUNT,
                  f"应有 {REQUEST_COUNT} 个响应，实际 {len(results)}")

            accepted = [row for row in results if row[0] == 200]
            rejected = [row for row in results if row[0] == 429]
            # 并行负载下晚到的连接可能在早到者完成后被放行，因此不锁"恰好 CAPACITY 条"；
            # 回归点是：容量必须真的可用（≥CAPACITY），且容量之外必须至少有一条 429。
            check(len(accepted) >= CAPACITY,
                  f"容量 {CAPACITY} 必须至少放行 {CAPACITY} 条，实际 {len(accepted)}；"
                  f"状态分布={sorted(status for status, _, _ in results)}")
            check(len(rejected) >= 1,
                  f"容量之外必须收到 429，实际 {len(rejected)}；"
                  f"状态分布={sorted(status for status, _, _ in results)}")
            check(len(accepted) + len(rejected) == REQUEST_COUNT,
                  f"响应必须全部归入放行/429，实际 {len(accepted)} + {len(rejected)}")
            for status, body, headers in rejected:
                check(headers.get("Retry-After") == "1", "429 必须带 Retry-After: 1")
                error = json.loads(body)["error"]
                check(error["code"] == "QUEUE_FULL", f"错误码应为 QUEUE_FULL：{error}")
                check(f"/{CAPACITY}" in error["message"],
                      f"QueueFull message 应带深度/容量 /{CAPACITY}：{error['message']}")

            status, body, _ = kit.get(port, "/metrics")
            check(status == 200, "/metrics 应 200")
            samples, _, _, _ = kit.parse_metrics(body.decode())
            check(samples['llmopt_requests_total{status="queue_full"}'] == len(rejected),
                  "queue_full 计数应等于 429 数")
            check(samples['llmopt_admission_rejected_total{reason="queue"}'] == len(rejected),
                  "准入拒绝计数应等于 429 数")
            check(samples['llmopt_requests_total{status="accepted"}'] == len(accepted),
                  "accepted 应等于放行数")
            print("OK integration_server_queue_capacity")
    # 第二段：任务队列必须有界，且队列满时优先回 503 而不是静默关连接。
    # C=1/Q=1 时 handler 5 个 + 1 条正常等待 + 1 条 shed 位；第 7 条连接应拿到
    # 503 SERVICE_UNAVAILABLE + Retry-After，第 8 条之后（超出 hard cap）才由传输层关闭。
    shed_queue = 1
    shed_count = 20
    with tempfile.TemporaryDirectory() as tmp:
        model = Path(tmp) / "fake.gguf"
        kit.make_gguf(model)
        with kit.running_server(
                server, model,
                ["--ctx-tokens", "512", "--max-concurrency", "1",
                 "--queue-capacity", str(shed_queue), "--chunk-size", "32",
                 "--max-num-batched-tokens", "32", "--log-level", "warn"],
                {"LLMOPT_TEST_STUB_DELAY_MS": "600"}) as port:
            start = threading.Barrier(shed_count)
            shed_results: list[tuple[int, bytes, dict]] = []
            lock = threading.Lock()

            def fire_shed(index: int) -> None:
                start.wait()
                try:
                    outcome = kit.post(port, "/completion",
                                       {"prompt": f"shed-{index}", "stream": True,
                                        "max_tokens": 2, "seed": index})
                except Exception as exc:  # noqa: BLE001 - hard cap 之外就是传输层关闭
                    outcome = (0, repr(exc).encode(), {})
                with lock:
                    shed_results.append(outcome)

            threads = [threading.Thread(target=fire_shed, args=(index,))
                       for index in range(shed_count)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(timeout=60)
            check(len(shed_results) == shed_count,
                  f"应有 {shed_count} 个结果，实际 {len(shed_results)}")
            statuses = sorted(status for status, _, _ in shed_results)
            accepted = statuses.count(200)
            rejected = statuses.count(429)
            unavailable = statuses.count(503)
            closed = statuses.count(0)
            check(unavailable >= 1,
                  f"队列满必须返回 503，实际 {unavailable}；状态分布={statuses}")
            check(closed >= 1,
                  f"hard cap 之外必须由传输层关闭，实际 {closed}；状态分布={statuses}")
            check(rejected >= 1,
                  f"容量内仍必须发 429，实际 {rejected}；状态分布={statuses}")
            check(accepted + rejected + unavailable + closed == shed_count,
                  f"结果必须全部归类，实际 {accepted}+{rejected}+{unavailable}+{closed}")
            for status, body, headers in shed_results:
                if status != 503:
                    continue
                check(headers.get("Retry-After") == "1", "503 必须带 Retry-After: 1")
                error = json.loads(body)["error"]
                check(error["code"] == "SERVICE_UNAVAILABLE",
                      f"传输层过载错误码应为 SERVICE_UNAVAILABLE：{error}")
            status, _, _ = kit.get(port, "/healthz")
            check(status == 200, "被传输层拒绝后服务必须仍然存活")
            print("OK integration_server_queue_bounded")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
