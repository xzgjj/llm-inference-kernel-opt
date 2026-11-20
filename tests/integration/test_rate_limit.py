# test_rate_limit.py — 6-G4：令牌桶开启后超额 429 且带 Retry-After，回填后恢复。
from __future__ import annotations

import json
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import server_kit as kit  # noqa: E402


def main() -> int:
    server = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as tmp:
        model = Path(tmp) / "ratelimit.gguf"
        kit.make_gguf(model)
        with kit.running_server(
            server, model,
            ["--rate-limit-rps", "2", "--rate-limit-burst", "2"],
        ) as port:
            payload = {"prompt": "hello", "max_tokens": 2, "seed": 1}
            results = []
            for _ in range(3):
                status, body, headers = kit.post(port, "/v1/completions", payload)
                results.append((status, body, headers))
            assert [item[0] for item in results] == [200, 200, 429], [i[0] for i in results]
            rejected = json.loads(results[2][1])
            assert rejected["error"]["code"] == "RATE_LIMITED", rejected
            assert rejected["error"]["retryable"] is True, rejected
            assert results[2][2].get("Retry-After") == "1", results[2][2]
            time.sleep(0.6)
            status, body, _ = kit.post(port, "/v1/completions", payload)
            assert status == 200, (status, body)
            status, metrics, _ = kit.get(port, "/metrics")
            assert status == 200
            samples, types, helps, dup = kit.parse_metrics(metrics.decode("utf-8"))
            assert not dup, dup
            assert samples["llmopt_rate_limited_total"] == 1, samples["llmopt_rate_limited_total"]
            assert types["llmopt_rate_limited_total"] == "counter"
            assert helps["llmopt_rate_limited_total"]
    print("rate limit ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
