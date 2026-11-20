#!/usr/bin/env python3
"""check_prefix.py — 真实后端前缀复用验收。

判定口径（与 SPEC-2 2-G6 对齐）：同前缀二次请求命中既有块，命中 token 数
大于 0，且第二次 prefill 的 token 数正好减少了命中的数量；KV 对账零差异。
不比较两次生成的 token 序列：部分前缀复用会改变同批计算的形状，llama 的
浮点归约顺序随之变化，贪心解码在长序列末端可能出现数值性翻转，这不属于
前缀块语义错误；块语义由 stub 后端的确定性用例覆盖。
"""
from __future__ import annotations

import argparse
import http.client
import json


def stream(port: int, prompt: str, max_tokens: int) -> list[int]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=120)
    try:
        body = json.dumps({"prompt": prompt, "stream": True, "max_tokens": max_tokens,
                           "seed": 7, "temperature": 0.0, "top_k": 1,
                           "top_p": 1.0, "min_p": 0.0, "repeat_penalty": 1.0})
        conn.request("POST", "/completion", body, {"Content-Type": "application/json"})
        response = conn.getresponse()
        if response.status != 200:
            raise RuntimeError(f"HTTP {response.status}")
        ids: list[int] = []
        while True:
            line = response.readline()
            if not line:
                break
            text = line.decode("utf-8", "replace").strip()
            if not text.startswith("data: "):
                continue
            payload = text[len("data: "):]
            if payload == "[DONE]":
                break
            ids.append(json.loads(payload)["choices"][0]["token_id"])
        return ids
    finally:
        conn.close()


def metric(port: int, name: str) -> int:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
    try:
        conn.request("GET", "/metrics")
        body = conn.getresponse().read().decode()
    finally:
        conn.close()
    for line in body.splitlines():
        if line.startswith(name + " "):
            return int(line.rsplit(" ", 1)[1])
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--tokens", type=int, default=16)
    parser.add_argument("--expect-share", type=int, default=1,
                        help="1=应命中前缀；0=后端不支持共享，只验不崩与零对账差异")
    args = parser.parse_args()
    prompt = "连续批处理的核心收益来自把多个请求的 prefill 与 decode 合成一个前向批。"

    prefill0 = metric(args.port, "llmopt_prefill_tokens_total")
    first_ids = stream(args.port, prompt, args.tokens)
    prefill1 = metric(args.port, "llmopt_prefill_tokens_total")
    second_ids = stream(args.port, prompt, args.tokens)
    prefill2 = metric(args.port, "llmopt_prefill_tokens_total")
    hits = metric(args.port, "llmopt_prefix_hits_total")
    hit_tokens = metric(args.port, "llmopt_prefix_hit_tokens_total")
    diffs = metric(args.port, "llmopt_kv_reconcile_diffs_total")

    saved = (prefill1 - prefill0) - (prefill2 - prefill1)
    print(f"hits={hits} hit_tokens={hit_tokens} prefill={prefill1 - prefill0}/"
          f"{prefill2 - prefill1} saved={saved} diffs={diffs}")
    if len(first_ids) != args.tokens or len(second_ids) != args.tokens:
        raise AssertionError(f"生成不完整: {len(first_ids)}/{len(second_ids)}")
    if args.expect_share == 0:
        if hits != 0:
            raise AssertionError(f"后端不支持共享却出现命中: hits={hits}")
        if diffs != 0:
            raise AssertionError(f"KV 对账存在 {diffs} 处差异")
        print("OK prefix-share=off（无命中、无崩溃、零对账差异）")
        return 0
    if hits < 1 or hit_tokens <= 0:
        raise AssertionError(f"同前缀未命中: hits={hits}, tokens={hit_tokens}")
    if saved != hit_tokens:
        raise AssertionError(f"prefill 节省 {saved} 与命中 {hit_tokens} 不一致")
    if diffs != 0:
        raise AssertionError(f"KV 对账存在 {diffs} 处差异")
    print(f"OK prefix hits={hits} tokens={hit_tokens} saved={saved}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
