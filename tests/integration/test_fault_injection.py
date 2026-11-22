"""test_fault_injection.py — D2-11/D2-06：五类故障面各一条最小注入用例。

覆盖（与 SPEC-DEBUG2 D2-11、D2-06 对齐）：
  1. runner 挂死：stub 步进延迟 500 ms + --completion-timeout-ms 100 → 请求失败但进程存活；
  2. 慢客户端：流式读到一半断开 → 服务端不崩、/readyz 仍 200；
  3. TLS 配置错：单独文件 test_server_tls_config_error.py 已覆盖，这里只做交叉引用；
  4. 磁盘满（用"dump 目录不可建"近似）：--slow-dump-dir 指向普通文件 → 请求仍 200；
  5. 请求时限到点：1 s deadline + 慢步进 → 504 TIMEOUT，进程仍可服务。
"""
from __future__ import annotations

import http.client
import json
import socket
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import server_kit as kit


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def readyz_ok(port: int) -> bool:
    try:
        status, _, _ = kit.get(port, "/readyz")
        return status == 200
    except OSError:
        return False


def wait_ready_again(port: int, seconds: float = 10.0) -> bool:
    deadline = time.time() + seconds
    while time.time() < deadline:
        if readyz_ok(port):
            return True
        time.sleep(0.2)
    return False


def main() -> int:
    server = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        model = root / "fake.gguf"
        kit.make_gguf(model)

        # 1) runner 挂死：单步延迟远大于 completion 回投超时。
        with kit.running_server(server, model,
                                ["--ctx-tokens", "512", "--max-concurrency", "1",
                                 "--max-num-batched-tokens", "256", "--log-level", "warn",
                                 "--completion-timeout-ms", "100"],
                                env_extra={"LLMOPT_TEST_STUB_DELAY_MS": "500"}) as port:
            status, body, _ = kit.post(port, "/completion",
                                       {"prompt": "hang", "max_tokens": 2, "seed": 1})
            check(status >= 400, f"挂死后端应失败，实际 {status}")
            payload = json.loads(body)
            code = payload.get("error", payload).get("code")
            check(code == "EXECUTION_FAILED", f"错误码应为 EXECUTION_FAILED，实际 {code}")
            check(wait_ready_again(port), "挂死请求后进程必须仍可服务")

        # 2) 慢客户端：读到首个 data 帧就断开，服务端不能崩。
        with kit.running_server(server, model,
                                ["--ctx-tokens", "512", "--max-concurrency", "2",
                                 "--max-num-batched-tokens", "256", "--log-level", "warn"]) as port:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
            conn.request("POST", "/completion",
                         json.dumps({"prompt": "slow", "max_tokens": 32, "stream": True,
                                     "seed": 2}),
                         {"Content-Type": "application/json"})
            response = conn.getresponse()
            check(response.status == 200, f"流式响应应 200，实际 {response.status}")
            _ = response.read(64)
            conn.sock.close()  # 模拟客户端半路断连
            conn.close()
            check(wait_ready_again(port, 5.0), "慢客户端断开后服务端必须仍存活")

        # 4) 磁盘满近似：dump 目录指向一个普通文件，写盘必然失败。
        blocker = root / "not-a-dir"
        blocker.write_text("x", encoding="utf-8")
        with kit.running_server(server, model,
                                ["--ctx-tokens", "512", "--max-concurrency", "1",
                                 "--max-num-batched-tokens", "256", "--log-level", "warn",
                                 "--slow-request-ms", "1",
                                 "--slow-dump-dir", str(blocker)],
                                env_extra={"LLMOPT_TEST_STUB_DELAY_MS": "20"}) as port:
            status, _, _ = kit.post(port, "/completion",
                                    {"prompt": "disk", "max_tokens": 8, "seed": 3})
            check(status == 200, f"dump 写失败不应影响请求，实际 {status}")
            check(wait_ready_again(port, 5.0), "dump 写失败后进程必须仍存活")

        # 5) 请求时限到点：单步 400 ms、8 token 远超 1 s 总时限，必须以 TIMEOUT 收尾。
        with kit.running_server(server, model,
                                ["--ctx-tokens", "512", "--max-concurrency", "1",
                                 "--max-num-batched-tokens", "256", "--log-level", "warn",
                                 "--request-deadline-seconds", "1"],
                                env_extra={"LLMOPT_TEST_STUB_DELAY_MS": "400"}) as port:
            status, body, _ = kit.post(port, "/completion",
                                       {"prompt": "deadline", "max_tokens": 8, "seed": 4})
            check(status == 504, f"时限到点应回 504，实际 {status}")
            payload = json.loads(body)
            code = payload.get("error", payload).get("code")
            check(code == "TIMEOUT", f"错误码应为 TIMEOUT，实际 {code}")
            check(wait_ready_again(port, 5.0), "超时请求后进程必须仍可服务")

    # 3) TLS 配置错：由 integration_server_tls_config_error 覆盖，这里只确认文件存在。
    tls_test = Path(__file__).resolve().parent / "test_server_tls_config_error.py"
    check(tls_test.is_file(), "TLS 配置错用例文件缺失")

    print("OK integration_fault_injection")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
