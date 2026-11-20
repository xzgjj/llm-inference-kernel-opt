"""SSE 分帧与计时：把网络字节流切成 `data:` 帧，不依赖第三方库。

生产路径用 http.client 的响应流；单测直接喂 bytes/str，覆盖分片、CRLF、注释与 [DONE]。
"""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from typing import Iterable, Iterator


@dataclass
class StreamEvent:
    """一次 SSE 事件：原始 data 文本与到达时刻（单调时钟毫秒）。"""

    data: str
    t_ms: float


@dataclass
class TokenEvent:
    """内容帧解析结果；finish_reason 仅在终止内容帧出现。"""

    token_id: int | None
    text: str
    finish_reason: str | None = None


@dataclass
class StreamStats:
    """一个请求的流式统计；时间戳为单调时钟毫秒。"""

    t_enqueue_ms: float = 0.0
    t_first_ms: float | None = None
    token_ts_ms: list[float] = field(default_factory=list)
    t_end_ms: float | None = None
    n_content_frames: int = 0
    saw_done: bool = False


def split_sse_bytes(chunks: Iterable[bytes]) -> Iterator[str]:
    """把字节分片切成完整 data 负载；跨分片缓冲，忽略注释与 event 行。"""
    buffer = ""
    for chunk in chunks:
        if not chunk:
            continue
        buffer += chunk.decode("utf-8", errors="replace")
        while "\n\n" in buffer:
            block, buffer = buffer.split("\n\n", 1)
            payload = _data_of(block)
            if payload is not None:
                yield payload
    tail = _data_of(buffer)
    if tail is not None:
        yield tail


def split_sse_text(text: str) -> Iterator[str]:
    """纯文本入口，便于单测；与字节入口共用同一解析规则。"""
    return split_sse_bytes([text.encode("utf-8")])


def data_of(block: str) -> str | None:
    """从一条 SSE 事件块取出 data 负载；无 data 行返回 None。"""
    lines = [line.rstrip("\r") for line in block.split("\n")]
    data_lines = [line[5:].lstrip() for line in lines if line.startswith("data:")]
    if not data_lines:
        return None
    return "\n".join(data_lines)


def _data_of(block: str) -> str | None:
    """兼容内部调用点的旧名。"""
    return data_of(block)


def parse_content_frame(payload: str) -> TokenEvent | None:
    """解析内容帧：兼容 llama.cpp legacy（content/tokens）与 OpenAI choices 两种格式。"""
    if payload.strip() == "[DONE]":
        return None
    try:
        obj = json.loads(payload)
    except json.JSONDecodeError:
        return None
    if "content" in obj and "tokens" in obj:
        tokens = obj.get("tokens") or []
        token_id = int(tokens[0]) if tokens else None
        finish = "stop" if obj.get("stop") else None
        return TokenEvent(token_id=token_id, text=str(obj.get("content") or ""), finish_reason=finish)
    choices = obj.get("choices") or []
    if not choices:
        return None
    choice = choices[0]
    token_id = choice.get("token_id")
    text = choice.get("text") or ""
    finish = choice.get("finish_reason")
    return TokenEvent(
        token_id=int(token_id) if token_id is not None else None,
        text=text,
        finish_reason=str(finish) if finish else None,
    )


def observe_payload(stats: StreamStats, payload: str, now_ms: float) -> None:
    """把一帧并入统计：首内容帧记 TTFT，后续内容帧记 ITL 时间戳。"""
    if payload.strip() == "[DONE]":
        stats.saw_done = True
        stats.t_end_ms = now_ms
        return
    event = parse_content_frame(payload)
    if event is None:
        return
    if event.token_id is None and not event.text:
        # 仅带 stop 的终止帧不计入 token 序列，只记结束时刻。
        if event.finish_reason:
            stats.t_end_ms = now_ms
        return
    stats.n_content_frames += 1
    if stats.t_first_ms is None:
        stats.t_first_ms = now_ms
    stats.token_ts_ms.append(now_ms)
    if event.finish_reason:
        stats.t_end_ms = now_ms


def monotonic_ms() -> float:
    """单调时钟毫秒，避免系统时间跳变影响延迟统计。"""
    return time.perf_counter() * 1000.0
