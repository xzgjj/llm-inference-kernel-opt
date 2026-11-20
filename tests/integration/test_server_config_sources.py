# test_server_config_sources.py — 6-G2：启动时逐项打印来源；CLI > env > file > default。
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
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
        port = kit.free_port()
        config = {
            "model": str(model),
            "host": "127.0.0.1",
            "port": kit.free_port(),  # 应被 CLI 覆盖，不参与监听
            "ctx_tokens": 1024,
            "queue_capacity": 8,
            "backend": "stub",
        }
        config_path = Path(tmp) / "config.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        env = dict(os.environ)
        env.update({"LLMOPT_ALLOW_STUB": "1", "LLMOPT_CTX_TOKENS": "2048"})
        proc = subprocess.Popen(
            [str(server), "--config", str(config_path), "--port", str(port)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        try:
            kit.wait_ready(port, proc)
            time.sleep(0.1)
        finally:
            proc.terminate()
            try:
                out, err = proc.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                out, err = proc.communicate(timeout=5)
        text = out.decode("utf-8", "replace")
        check("config: model=" in text, f"缺少 config 摘要\n{text}\n{err!r}")
        check("config source: port=cli" in text, f"port 来源应为 cli\n{text}")
        check("config source: ctx-tokens=env" in text, f"ctx 来源应为 env\n{text}")
        check("config source: model=file" in text, f"model 来源应为 file\n{text}")
        check("config source: queue-capacity=file" in text, f"queue 来源应为 file\n{text}")
        check("config source: block-size=default" in text, f"未设置项应为 default\n{text}")
        print("OK integration_server_config_sources")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
