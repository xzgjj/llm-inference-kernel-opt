"""HTTP/SSE 客户端：单请求计时、健康检查、tokenize/detokenize 辅助。

只依赖标准库；每个请求独立连接，避免连接复用的排队误差污染 TTFT。
"""

from __future__ import annotations

import http.client
import json
import socket
from dataclasses import dataclass
from typing import Any

from . import sse


@dataclass
class RequestResult:
    """单请求结果；error_code 为空表示成功。"""

    stats: sse.StreamStats
    status: str
    error_code: str | None
    n_prompt: int
    n_gen: int
    text: str


def _post_json(
    host: str,
    port: int,
    path: str,
    payload: dict,
    timeout_s: float,
) -> tuple[int, bytes]:
    conn = http.client.HTTPConnection(host, port, timeout=timeout_s)
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    try:
        conn.request("POST", path, body=body, headers={"Content-Type": "application/json"})
        resp = conn.getresponse()
        data = resp.read()
        return resp.status, data
    finally:
        conn.close()


def _get(host: str, port: int, path: str, timeout_s: float) -> tuple[int, bytes]:
    conn = http.client.HTTPConnection(host, port, timeout=timeout_s)
    try:
        conn.request("GET", path)
        resp = conn.getresponse()
        return resp.status, resp.read()
    finally:
        conn.close()


def health_ok(host: str, port: int, timeout_s: float = 2.0) -> bool:
    try:
        status, _ = _get(host, port, "/health", timeout_s)
        return status < 500
    except (OSError, http.client.HTTPException):
        return False


def tokenize(host: str, port: int, text: str, timeout_s: float = 10.0) -> list[int]:
    """调用 llama-server /tokenize；返回 token id 列表。"""
    status, data = _post_json(host, port, "/tokenize", {"content": text}, timeout_s)
    if status != 200:
        raise RuntimeError(f"/tokenize 返回 {status}")
    return [int(t) for t in json.loads(data).get("tokens", [])]


def detokenize(host: str, port: int, tokens: list[int], timeout_s: float = 10.0) -> str:
    status, data = _post_json(host, port, "/detokenize", {"tokens": tokens}, timeout_s)
    if status != 200:
        raise RuntimeError(f"/detokenize 返回 {status}")
    return str(json.loads(data).get("content", ""))


def run_stream(
    host: str,
    port: int,
    payload: dict,
    n_prompt: int,
    timeout_s: float = 120.0,
) -> RequestResult:
    """发一次流式请求并逐帧计时；断连/超时都落成 error 记录而不是抛出。"""
    stats = sse.StreamStats()
    conn = http.client.HTTPConnection(host, port, timeout=timeout_s)
    text_parts: list[str] = []
    try:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        stats.t_enqueue_ms = sse.monotonic_ms()
        conn.request(
            "POST",
            "/completion",
            body=body,
            headers={"Content-Type": "application/json", "Accept": "text/event-stream"},
        )
        resp = conn.getresponse()
        if resp.status != 200:
            return RequestResult(
                stats=stats,
                status="error",
                error_code=f"http_{resp.status}",
                n_prompt=n_prompt,
                n_gen=0,
                text="",
            )
        for block in _iter_blocks(resp, timeout_s):
            payload_text = sse.data_of(block)
            if payload_text is None:
                continue
            now = sse.monotonic_ms()
            event = sse.parse_content_frame(payload_text)
            if event is not None:
                text_parts.append(event.text)
            sse.observe_payload(stats, payload_text, now)
        if stats.t_end_ms is None:
            stats.t_end_ms = sse.monotonic_ms()
        status = "ok" if stats.n_content_frames else "error"
        return RequestResult(
            stats=stats,
            status=status,
            error_code=None if status == "ok" else "empty_stream",
            n_prompt=n_prompt,
            n_gen=stats.n_content_frames,
            text="".join(text_parts),
        )
    except socket.timeout:
        return _failed(stats, n_prompt, "timeout")
    except (OSError, http.client.HTTPException) as exc:
        return _failed(stats, n_prompt, f"stream_{type(exc).__name__.lower()}")
    finally:
        conn.close()


def _failed(stats: sse.StreamStats, n_prompt: int, code: str) -> RequestResult:
    stats.t_end_ms = stats.t_end_ms or sse.monotonic_ms()
    return RequestResult(stats, "error", code, n_prompt, 0, "")


def _iter_blocks(resp: http.client.HTTPResponse, timeout_s: float) -> Any:
    """按空行切 SSE 块；行到达时立即返回，保证逐 token 时间戳真实。"""
    del timeout_s
    block: list[str] = []
    while True:
        line = resp.readline()
        if not line:
            if block:
                yield "\n".join(block)
            return
        text = line.decode("utf-8", errors="replace").rstrip("\r\n")
        if text == "":
            if block:
                yield "\n".join(block)
                block = []
            continue
        block.append(text)
