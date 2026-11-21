# test_server_tls_config_error.py — 配置 TLS 但证书不可用/构建未启用 TLS 时必须启动失败。
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import server_kit as kit


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    server = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as tmp:
        model = Path(tmp) / "fake.gguf"
        kit.make_gguf(model)
        env = dict(os.environ)
        env["LLMOPT_ALLOW_STUB"] = "1"
        proc = subprocess.run(
            [str(server), "--model", str(model), "--host", "127.0.0.1",
             "--port", str(kit.free_port()), "--backend", "stub", "--mode", "off",
             "--tls-cert", str(Path(tmp) / "missing.pem"),
             "--tls-key", str(Path(tmp) / "missing.key")],
            capture_output=True, text=True, env=env, timeout=30,
        )
        check(proc.returncode == 3, f"TLS 配置失败应退出 3，实际 {proc.returncode}")
        output = proc.stdout + proc.stderr
        check("TLS" in output, f"错误信息应说明 TLS 原因，实际：{output!r}")
        print("OK integration_server_tls_config_error")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
