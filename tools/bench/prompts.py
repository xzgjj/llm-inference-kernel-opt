"""固定种子的 prompt 集生成：按 token 上限逐句装箱，产出可归档、可复现的文本。

token 计数由调用方注入（生产走 llama-server /tokenize，单测走字符计数桩），
因此本模块不依赖网络，也不猜 tokenizer 词表。
"""

from __future__ import annotations

import hashlib
import json
import random
from dataclasses import dataclass
from typing import Callable

# 句子库刻意覆盖中英混排、标点与数字，避免 prompt 过于同质。
SENTENCE_BANK = [
    "连续批处理把不同请求的 decode 步骤合并到同一次前向。",
    "块表负责逻辑分页，物理 cell 由后端决定。",
    "首 token 延迟由排队、分词与前向三部分组成。",
    "固定种子下，同一请求的输出序列必须逐 token 一致。",
    "KV 显存占用随上下文长度线性增长。",
    "调度器的单步开销应当低于前向耗时的百分之五。",
    "量化权重减少了显存带宽压力，但可能损失少量精度。",
    "SSE 逐 token 回包便于观察生成节奏。",
    "取消请求后必须释放块与上下文槽，避免泄漏。",
    "吞吐与延迟是两条互相拉扯的曲线。",
    "短请求不应被长请求饿死，这是公平性的底线。",
    "prefill 阶段不需要采样，只有最后一段才置 logits。",
    "A token budget separates request count from sequence length.",
    "The scheduler owns sequence state; the runner owns the context.",
    "Prefix reuse turns repeated system prompts into shared blocks.",
    "Prefill chunks interleave with decode to smooth inter-token latency.",
    "Every measurement row carries run_id and concurrency for audit.",
    "A fixed seed makes sampling reproducible across two rounds.",
    "Block tables are the single source of truth for KV allocation.",
    "Memory headroom of twenty percent absorbs allocator fragmentation.",
]


@dataclass(frozen=True)
class TierSpec:
    name: str
    max_prompt_tokens: int
    n: int


def _sentence_sequence(rng: random.Random) -> list[str]:
    """打乱但可复现的句子顺序；顺序由 seed 决定。"""
    ordered = list(SENTENCE_BANK)
    rng.shuffle(ordered)
    return ordered


def build_prompt_set(
    seed: int,
    tiers: list[TierSpec],
    token_count: Callable[[str], int],
    reuse_each: int = 64,
    shared_prefix_tokens: int = 0,
) -> list[dict]:
    """按 tier 生成 prompt；每句累加后不超过 max_prompt_tokens。

    shared_prefix_tokens > 0 时，每条 prompt 共享同一段前缀（按档位上限预留最多 8 token
    的独有尾巴），用于测量前缀缓存的净收益；0 保持历史口径（全部唯一文本）。
    """
    rng = random.Random(seed)
    ordered = _sentence_sequence(rng)
    rows: list[dict] = []
    cursor = 0
    for tier in tiers:
        prefix = ""
        if shared_prefix_tokens > 0 and tier.max_prompt_tokens > 8:
            prefix_cap = min(shared_prefix_tokens, tier.max_prompt_tokens - 8)
            prefix = _build_prefix(ordered, prefix_cap, token_count)
        for index in range(tier.n):
            if prefix:
                # 共享前缀 + 每条唯一的短尾巴：前缀缓存能命中，样本又不是同一句。
                sentence = f"样本{index:03d}。" + ordered[cursor % len(ordered)]
                cursor += 1
                text = prefix + _unique_tail(sentence, prefix, tier.max_prompt_tokens, token_count)
                rows.append(
                    {
                        "prompt_id": f"{tier.name}-{index:03d}",
                        "tier": tier.name,
                        "text": text,
                        "max_prompt_tokens": tier.max_prompt_tokens,
                        "shared_prefix": True,
                    }
                )
                continue
            parts: list[str] = []
            while True:
                sentence = ordered[cursor % len(ordered)]
                cursor += 1
                candidate = "".join(parts) + sentence
                if parts and token_count(candidate) > tier.max_prompt_tokens:
                    break
                parts.append(sentence)
                if token_count("".join(parts)) > tier.max_prompt_tokens:
                    # 单句已超限：截断到上限附近，保证仍能产出该 tier 的样本。
                    parts = [sentence[: max(8, tier.max_prompt_tokens // 3)]]
                    break
                if len(parts) >= reuse_each:
                    break
            rows.append(
                {
                    "prompt_id": f"{tier.name}-{index:03d}",
                    "tier": tier.name,
                    "text": "".join(parts),
                    "max_prompt_tokens": tier.max_prompt_tokens,
                    "shared_prefix": False,
                }
            )
    return rows


def _build_prefix(ordered: list[str], cap: int, token_count: Callable[[str], int]) -> str:
    """按固定顺序拼一段不超过 cap 的前缀；所有请求复用同一段文本。"""
    parts: list[str] = []
    for sentence in ordered:
        candidate = "".join(parts) + sentence
        if parts and token_count(candidate) > cap:
            break
        parts.append(sentence)
        if token_count("".join(parts)) > cap:
            break
    text = "".join(parts)
    while text and token_count(text) > cap:
        text = text[:-1]
    return text


def _unique_tail(sentence: str, prefix: str, max_tokens: int,
                 token_count: Callable[[str], int]) -> str:
    """截出一段能塞进剩余预算的唯一尾巴；保证 prefix+tail 不超上限。"""
    text = sentence
    while text and token_count(prefix + text) > max_tokens:
        text = text[:-1]
    return text


def prompt_set_hash(rows: list[dict]) -> str:
    """稳定哈希：只覆盖 prompt_id、tier 与文本，便于跨轮对账。"""
    payload = json.dumps(
        [{"prompt_id": r["prompt_id"], "tier": r["tier"], "text": r["text"]} for r in rows],
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    )
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def char_token_count(text: str) -> int:
    """无 tokenizer 时的保守估算：按非空白字符计，只用于单测与离线草稿。"""
    return sum(1 for ch in text if not ch.isspace())
