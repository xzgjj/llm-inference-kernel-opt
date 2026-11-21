# test_server_tls.py — HTTPS 数据面：health/推理/指标均走 TLS，明文连接不得成功。
from __future__ import annotations

import http.client
import json
import ssl
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import server_kit as kit


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def make_cert(tmp: Path) -> tuple[Path, Path]:
    cert = tmp / "cert.pem"
    key = tmp / "key.pem"
    result = subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
         "-keyout", str(key), "-out", str(cert), "-subj", "/CN=127.0.0.1",
         "-addext", "subjectAltName=IP:127.0.0.1"],
        capture_output=True, text=True, timeout=30,
    )
    if result.returncode != 0:
        raise RuntimeError(f"openssl 生成证书失败：{result.stderr}")
    return cert, key


def main() -> int:
    server = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as tmp_dir:
        tmp = Path(tmp_dir)
        model = tmp / "fake.gguf"
        kit.make_gguf(model)
        try:
            cert, key = make_cert(tmp)
        except (FileNotFoundError, RuntimeError) as exc:
            print(f"SKIP：{exc}")
            return 77
        with kit.running_server_tls(server, model, [
                "--ctx-tokens", "512", "--max-concurrency", "1",
                "--max-num-batched-tokens", "256", "--log-level", "warn"],
                cert, key) as port:
            status, body, headers = kit.get_https(port, "/healthz")
            check(status == 200, f"HTTPS /healthz 应 200，实际 {status}")
            check(json.loads(body)["status"] == "ready", f"状态错误：{body!r}")

            status, body, _ = kit.post_https(
                port, "/completion",
                {"prompt": "tls", "max_tokens": 4, "seed": 1})
            check(status == 200, f"HTTPS /completion 应 200，实际 {status}")
            check(b"choices" in body, "HTTPS 推理响应应含 choices")

            status, body, headers = kit.get_https(port, "/metrics")
            check(status == 200, f"HTTPS /metrics 应 200，实际 {status}")
            check(b"llmopt_" in body, "HTTPS /metrics 应返回指标文本")
            check("text/plain" in headers.get("Content-Type", ""), "metrics Content-Type 错误")

            # 同一端口不开放明文；若 httplib 拒绝握手，客户端应抛异常或得到非 200。
            try:
                conn = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
                conn.request("GET", "/healthz")
                plain = conn.getresponse()
                conn.close()
                check(plain.status != 200, "TLS 端口不应接受明文 HTTP 200")
            except (OSError, ssl.SSLError):
                pass
        print("OK integration_server_tls")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
