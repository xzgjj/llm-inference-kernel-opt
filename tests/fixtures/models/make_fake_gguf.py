# make_fake_gguf.py — 生成只含元数据的最小 GGUF，供 stub 后端与进程级冒烟使用。
# 用法：python tests/fixtures/models/make_fake_gguf.py <输出路径>
from __future__ import annotations

import struct
import sys
from pathlib import Path


def string(text: str) -> bytes:
    """GGUF 字符串：u64 长度 + UTF-8 字节。"""
    return struct.pack("<Q", len(text.encode())) + text.encode()


def kv_u32(key: str, value: int) -> bytes:
    """一个 u32 类型的键值对。"""
    return string(key) + struct.pack("<I", 4) + struct.pack("<I", value)


def build() -> bytes:
    """2 层、4 头、KV 头 2、隐层 128 的最小头。"""
    body = struct.pack("<IIQQ", 0x46554747, 3, 0, 4)
    body += kv_u32("llama.block_count", 2)
    body += kv_u32("llama.attention.head_count", 4)
    body += kv_u32("llama.attention.head_count_kv", 2)
    body += kv_u32("llama.embedding_length", 128)
    return body


if __name__ == "__main__":
    target = Path(sys.argv[1] if len(sys.argv) > 1 else "fake.gguf")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(build())
    print(f"wrote {target} ({target.stat().st_size} bytes)")
