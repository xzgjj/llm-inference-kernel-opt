# test_compat_openai.py — 6-G1/6-G5：三端点兼容面、stop/n/seed、旧端点回归与请求 ID 日志。
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import server_kit as kit  # noqa: E402


def sse_frames(body: bytes) -> list[str]:
    """把 SSE 解码成帧列表；返回每个 data: 后的正文。"""
    text = body.decode("utf-8")
    frames = []
    for block in text.split("\n\n"):
        block = block.strip()
        if not block:
            continue
        assert block.startswith("data: "), block
        frames.append(block[len("data: "):])
    return frames


def post_json(port: int, path: str, payload: dict, request_id: str = "") -> tuple[int, dict]:
    import http.client

    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    headers = {"Content-Type": "application/json"}
    if request_id:
        headers["X-Request-Id"] = request_id
    try:
        conn.request("POST", path, json.dumps(payload), headers)
        response = conn.getresponse()
        return response.status, json.loads(response.read().decode("utf-8"))
    finally:
        conn.close()


def main() -> int:
    server = Path(sys.argv[1])
    evidence = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("evidence")
    evidence.mkdir(parents=True, exist_ok=True)
    workdir = Path(tempfile.mkdtemp(prefix="compat-"))
    model = workdir / "compat.gguf"
    kit.make_gguf(model)

    port = kit.free_port()
    env = dict(os.environ)
    env["LLMOPT_ALLOW_STUB"] = "1"
    env["LLMOPT_STEP_TRACE_RATE"] = "0"
    cmd = [str(server), "--model", str(model), "--host", "127.0.0.1", "--port", str(port),
           "--backend", "stub", "--mode", "off"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                            cwd=str(workdir))
    inference_posts = 0
    try:
        kit.wait_ready(port, proc)

        status, body, _ = kit.get(port, "/v1/models")
        assert status == 200, status
        models = json.loads(body)
        assert models["object"] == "list"
        assert models["data"][0]["id"] == "compat", models

        chat = {"model": "compat", "messages": [{"role": "user", "content": "你好"}],
                "max_tokens": 8, "seed": 0}
        status, result = post_json(port, "/v1/chat/completions", chat, "trace-chat-1")
        inference_posts += 1
        assert status == 200, (status, result)
        assert result["object"] == "chat.completion", result
        assert result["id"] == "trace-chat-1", result
        choice = result["choices"][0]
        assert choice["message"]["role"] == "assistant"
        assert choice["finish_reason"] == "length"
        assert result["usage"]["total_tokens"] > 0
        first_text = choice["message"]["content"]

        status, stream_body, _ = kit.post(port, "/v1/chat/completions",
                                          dict(chat, stream=True), timeout=30)
        inference_posts += 1
        assert status == 200, status
        frames = sse_frames(stream_body)
        assert frames[-1] == "[DONE]", frames[-1]
        content_frames = [json.loads(f) for f in frames[:-1]]
        assert len(content_frames) >= 2, len(content_frames)
        assert content_frames[0]["object"] == "chat.completion.chunk"
        assert content_frames[0]["choices"][0]["delta"]["content"]
        streamed = "".join(f["choices"][0]["delta"]["content"] for f in content_frames)
        assert streamed == first_text, (streamed, first_text)
        (evidence / "compat_frames.txt").write_text(
            "chat stream frames:\n" + "\n".join(frames) + "\n", encoding="utf-8")

        status, legacy, _ = kit.post(port, "/completion",
                                     {"prompt": "hello", "n_predict": 4, "seed": 0})
        inference_posts += 1
        assert status == 200, (status, legacy)
        legacy_body = json.loads(legacy)
        assert legacy_body["object"] == "text_completion"
        assert legacy_body["choices"][0]["finish_reason"] == "length"

        status, many = post_json(port, "/v1/completions",
                                 {"prompt": "hello", "max_tokens": 4, "n": 2, "seed": 0})
        inference_posts += 1
        assert status == 200, (status, many)
        assert len(many["choices"]) == 2, many
        assert [c["index"] for c in many["choices"]] == [0, 1]

        status, seeded = post_json(port, "/v1/completions",
                                   {"prompt": "hello", "max_tokens": 4, "seed": 0})
        inference_posts += 1
        status, other = post_json(port, "/v1/completions",
                                  {"prompt": "hello", "max_tokens": 4, "seed": 7})
        inference_posts += 1
        assert seeded["choices"][0]["text"] != other["choices"][0]["text"], "seed 未生效"

        stop_char = first_text[:1]
        if stop_char:
            status, stopped = post_json(port, "/v1/chat/completions",
                                        dict(chat, stop=[stop_char], max_tokens=16))
            inference_posts += 1
            assert status == 200, (status, stopped)
            choice = stopped["choices"][0]
            assert choice["finish_reason"] == "stop", choice
            assert len(choice["message"]["content"]) < 16, choice
            assert stop_char not in choice["message"]["content"], choice

        status, rejected = post_json(port, "/v1/chat/completions",
                                     dict(chat, tools=[{"type": "function"}]))
        inference_posts += 1
        assert status == 400, (status, rejected)
        assert rejected["error"]["code"] == "unsupported_field", rejected

        status, metrics_body, _ = kit.get(port, "/metrics")
        assert status == 200
        samples, types, helps, dup = kit.parse_metrics(metrics_body.decode("utf-8"))
        assert not dup, dup
        assert "llmopt_http_requests_total" in samples
        assert samples["llmopt_http_requests_total"] == inference_posts, (
            samples["llmopt_http_requests_total"], inference_posts)
        for family in ("llmopt_http_requests_total", "llmopt_requests_total"):
            assert types.get(family) == "counter", (family, types.get(family))
            assert helps.get(family), family
        assert samples["llmopt_requests_total{status=\"ok\"}"] >= 1
        (evidence / "metrics_format.txt").write_text(metrics_body.decode("utf-8"),
                                                     encoding="utf-8")
    finally:
        proc.terminate()
        try:
            out, _ = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate(timeout=5)
        log_text = (out or b"").decode("utf-8", errors="replace")
        (evidence / "server_stdout.log").write_text(log_text, encoding="utf-8")
        assert "trace-chat-1" in log_text, log_text[-4000:]
        logged = log_text.count("http request id=") + log_text.count("http reject id=")
        assert logged >= inference_posts, (logged, inference_posts, log_text[-4000:])
    print(f"compat openai ok: posts={inference_posts} port={port}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
