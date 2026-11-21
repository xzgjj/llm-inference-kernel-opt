"""阶段 0 prompt 集：确定性、上限与归档哈希。"""

from __future__ import annotations

import unittest

from tools.bench import prompts


TIERS = [
    prompts.TierSpec("short", 128, 3),
    prompts.TierSpec("long", 512, 3),
]


class PromptSetTest(unittest.TestCase):
    def test_counts_and_tiers(self) -> None:
        rows = prompts.build_prompt_set(42, TIERS, prompts.char_token_count)
        self.assertEqual(len(rows), 6)
        self.assertEqual({r["tier"] for r in rows}, {"short", "long"})
        self.assertEqual(len({r["prompt_id"] for r in rows}), 6)

    def test_each_prompt_respects_cap(self) -> None:
        rows = prompts.build_prompt_set(42, TIERS, prompts.char_token_count)
        for row in rows:
            self.assertLessEqual(
                prompts.char_token_count(row["text"]), row["max_prompt_tokens"], row["prompt_id"]
            )

    def test_same_seed_same_hash(self) -> None:
        first = prompts.build_prompt_set(7, TIERS, prompts.char_token_count)
        second = prompts.build_prompt_set(7, TIERS, prompts.char_token_count)
        self.assertEqual(prompts.prompt_set_hash(first), prompts.prompt_set_hash(second))

    def test_different_seed_changes_order(self) -> None:
        first = prompts.build_prompt_set(7, TIERS, prompts.char_token_count)
        second = prompts.build_prompt_set(8, TIERS, prompts.char_token_count)
        self.assertNotEqual(prompts.prompt_set_hash(first), prompts.prompt_set_hash(second))

    def test_shared_prefix_keeps_unique_tail_and_cap(self) -> None:
        """前缀缓存对照用：所有 prompt 共享同一前缀，但每条仍有唯一尾巴且不超上限。"""
        tiers = [prompts.TierSpec("short", 128, 5)]
        rows = prompts.build_prompt_set(
            42, tiers, prompts.char_token_count, shared_prefix_tokens=64
        )
        texts = [row["text"] for row in rows]
        self.assertTrue(all(row["shared_prefix"] for row in rows))
        self.assertEqual(len(set(texts)), 5)
        self.assertTrue(all(text.startswith(texts[0][:24]) for text in texts))
        for row in rows:
            self.assertLessEqual(
                prompts.char_token_count(row["text"]), row["max_prompt_tokens"], row["prompt_id"]
            )

    def test_zero_shared_prefix_marks_rows_unique(self) -> None:
        rows = prompts.build_prompt_set(42, TIERS, prompts.char_token_count)
        self.assertFalse(any(row["shared_prefix"] for row in rows))
        self.assertEqual(len({row["text"] for row in rows}), len(rows))


if __name__ == "__main__":
    unittest.main()
