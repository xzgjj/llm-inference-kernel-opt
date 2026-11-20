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


if __name__ == "__main__":
    unittest.main()
