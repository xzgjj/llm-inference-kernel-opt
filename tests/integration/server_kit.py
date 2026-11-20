# server_kit.py — 集成用例共用：最小 GGUF、空闲端口、就绪等待、prom 解析。
# 只依赖标准库；由 CTest 以脚本路径启动，sys.path[0] 即本目录。
from __future__ import annotations

import hashlib
import http.client
import json
import os
import socket
import struct
import subprocess
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def make_gguf(path: Path, block_count: int = 2, head_count: int = 4,
              head_count_kv: int = 2, embedding_length: int = 128) -> None:
    """写最小 GGUF 头；门控估算只需要这几个元数据。"""

    def string(text: str) -> bytes:
        return struct.pack("<Q", len(text.encode())) + text.encode()

    def kv_u32(key: str, value: int) -> bytes:
        return string(key) + struct.pack("<I", 4) + struct.pack("<I", value)

    body = struct.pack("<IIQQ", 0x46554747, 3, 0, 4)
    body += kv_u32("llama.block_count", block_count)
    body += kv_u32("llama.attention.head_count", head_count)
    body += kv_u32("llama.attention.head_count_kv", head_count_kv)
    body += kv_u32("llama.embedding_length", embedding_length)
    path.write_bytes(body)


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def post(port: int, path: str, payload: dict, timeout: float = 30.0) -> tuple[int, bytes, dict]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    try:
        conn.request("POST", path, json.dumps(payload), {"Content-Type": "application/json"})
        response = conn.getresponse()
        body = response.read()
        return response.status, body, dict(response.getheaders())
    finally:
        conn.close()


def get(port: int, path: str, timeout: float = 10.0) -> tuple[int, bytes, dict]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    try:
        conn.request("GET", path)
        response = conn.getresponse()
        return response.status, response.read(), dict(response.getheaders())
    finally:
        conn.close()


def wait_ready(port: int, proc: subprocess.Popen[bytes], timeout: float = 30.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            out, err = proc.communicate()
            raise RuntimeError(f"服务提前退出 rc={proc.returncode}\n{out}\n{err}")
        try:
            status, _, _ = get(port, "/healthz")
            if status == 200:
                return
        except OSError:
            pass
        time.sleep(0.1)
    raise RuntimeError("等待 /healthz 就绪超时")


@contextmanager
def running_server(server: Path, model: Path, args: list[str],
                   env_extra: dict | None = None) -> Iterator[int]:
    """起一个 stub 服务，退出时确保回收；端口由内核分配。"""
    port = free_port()
    env = dict(os.environ)
    env["LLMOPT_ALLOW_STUB"] = "1"
    if env_extra:
        env.update(env_extra)
    cmd = [str(server), "--model", str(model), "--host", "127.0.0.1", "--port", str(port),
           "--backend", "stub", "--mode", "off"] + args
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    try:
        wait_ready(port, proc)
        yield port
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


def parse_metrics(text: str) -> tuple[dict[str, float], dict[str, str], dict[str, str], list[str]]:
    """解析 Prometheus 文本；返回 samples/types/helps 与重复 HELP 的 family 列表。"""
    samples: dict[str, float] = {}
    types: dict[str, str] = {}
    helps: dict[str, str] = {}
    duplicate_help: list[str] = []
    for line in text.splitlines():
        if not line or line.startswith("#"):
            if line.startswith("# TYPE "):
                _, _, family, kind = line.split(" ", 3)
                types[family] = kind
            elif line.startswith("# HELP "):
                _, _, family, help_text = line.split(" ", 3)
                if family in helps:
                    duplicate_help.append(family)
                helps[family] = help_text
            continue
        name, value = line.rsplit(" ", 1)
        samples[name] = float("nan") if value == "NaN" else float(value)
    return samples, types, helps, duplicate_help
