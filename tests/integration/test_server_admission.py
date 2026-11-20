# test_server_admission.py — 4-G0-6 HTTP 契约：队列满回 429 + Retry-After，且只记一次。
# 通过 LLMOPT_TEST_STUB_DELAY_MS 放慢假后端，让“突发 8 条、预算 3 条”的竞态可复现。
from __future__ import annotations

import json
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import server_kit as kit

REQUEST_COUNT = 8
BUDGET = 3  # max_concurrency=1 + queue_capacity=2


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
                ["--ctx-tokens", "512", "--max-concurrency", "1", "--queue-capacity", "2",
                 "--chunk-size", "32", "--max-num-batched-tokens", "32", "--log-level", "warn"],
                {"LLMOPT_TEST_STUB_DELAY_MS": "25"}) as port:
            start = threading.Barrier(REQUEST_COUNT)
            results: list[tuple[int, bytes, dict]] = []
            lock = threading.Lock()

            def fire(index: int) -> None:
                start.wait()
                try:
                    outcome = kit.post(port, "/completion",
                                       {"prompt": f"admission-{index}", "stream": True,
                                        "max_tokens": 16, "seed": index})
                except OSError as exc:  # 连接层失败同样是拒绝/错误，按原样记录
                    outcome = (0, str(exc).encode(), {})
                with lock:
                    results.append(outcome)

            threads = [threading.Thread(target=fire, args=(index,))
                       for index in range(REQUEST_COUNT)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(timeout=30)
            check(len(results) == REQUEST_COUNT, f"应有 {REQUEST_COUNT} 个响应，实际 {len(results)}")

            accepted = [row for row in results if row[0] == 200]
            rejected = [row for row in results if row[0] == 429]
            check(len(accepted) == BUDGET, f"预算 {BUDGET} 应恰好放行 {BUDGET} 条，实际 {len(accepted)}")
            check(len(rejected) == REQUEST_COUNT - BUDGET,
                  f"其余应全部 429，实际 {len(rejected)}")
            for status, body, headers in rejected:
                check(headers.get("Retry-After") == "1", "429 必须带 Retry-After: 1")
                error = json.loads(body)["error"]
                check(error["code"] == "QUEUE_FULL", f"错误码应为 QUEUE_FULL，实际 {error['code']}")

            status, body, _ = kit.get(port, "/metrics")
            check(status == 200, "/metrics 应 200")
            samples, _, _, _ = kit.parse_metrics(body.decode())
            rejected_count = len(rejected)
            check(samples['llmopt_requests_total{status="queue_full"}'] == rejected_count,
                  "queue_full 计数应等于 429 数")
            check(samples["llmopt_admission_rejected_total{reason=\"queue\"}"] == rejected_count,
                  "准入拒绝计数应等于 429 数")
            check(samples['llmopt_requests_total{status="accepted"}'] == len(accepted),
                  "accepted 应等于放行数")
            check(samples['llmopt_requests_total{status="error"}'] == 0,
                  "429 不得再被记成执行错误")

            # 退避后重试必须成功：429 只表示限流，不改变服务可用性。
            time.sleep(0.2)
            status, body, _ = kit.post(port, "/completion",
                                       {"prompt": "retry-after", "stream": True,
                                        "max_tokens": 2, "seed": 99})
            check(status == 200, f"退避后重试应 200，实际 {status}")
            print("OK integration_server_admission")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
