"""SSE 分帧与计时：分片、CRLF、[DONE] 与首帧时间戳。"""

from __future__ import annotations

import unittest

from tools.bench import sse


def frame(text: str, token_id: int, finish: str | None = None) -> str:
    import json

    payload = {
        "choices": [
            {"index": 0, "text": text, "token_id": token_id, "finish_reason": finish}
        ]
    }
    return "data: " + json.dumps(payload, ensure_ascii=False) + "\n\n"


class SseTest(unittest.TestCase):
    def test_split_across_chunks(self) -> None:
        text = frame("连续", 1) + frame("批", 2, "stop") + "data: [DONE]\n\n"
        cut = text.index("批")
        chunks = [text[:cut].encode(), text[cut:].encode()]
        self.assertEqual(list(sse.split_sse_bytes(chunks))[2], "[DONE]")

    def test_crlf_and_comment(self) -> None:
        text = ": keep-alive\r\n\r\n" + frame("好", 3).replace("\n", "\r\n")
        payloads = list(sse.split_sse_text(text))
        self.assertEqual(len(payloads), 1)
        self.assertIn("\"token_id\": 3", payloads[0])

    def test_done_is_not_content(self) -> None:
        stats = sse.StreamStats(t_enqueue_ms=100.0)
        sse.observe_payload(stats, "[DONE]", 200.0)
        self.assertTrue(stats.saw_done)
        self.assertEqual(stats.t_end_ms, 200.0)
        self.assertEqual(stats.n_content_frames, 0)

    def test_first_token_and_itl_timestamps(self) -> None:
        stats = sse.StreamStats(t_enqueue_ms=100.0)
        sse.observe_payload(stats, sse.data_of(frame("甲", 1)), 130.0)
        sse.observe_payload(stats, sse.data_of(frame("乙", 2)), 160.0)
        self.assertEqual(stats.t_first_ms, 130.0)
        self.assertEqual(stats.token_ts_ms, [130.0, 160.0])

    def test_llama_legacy_frames(self) -> None:
        legacy = (
            'data: {"index":0,"content":"你","tokens":[7],"stop":false}\n\n'
            'data: {"index":0,"content":"","tokens":[],"stop":true}\n\n'
        )
        payloads = list(sse.split_sse_text(legacy))
        stats = sse.StreamStats(t_enqueue_ms=0.0)
        sse.observe_payload(stats, payloads[0], 20.0)
        sse.observe_payload(stats, payloads[1], 40.0)
        self.assertEqual(stats.n_content_frames, 1)
        self.assertEqual(stats.token_ts_ms, [20.0])
        self.assertEqual(stats.t_end_ms, 40.0)

    def test_bad_json_is_skipped(self) -> None:
        stats = sse.StreamStats()
        sse.observe_payload(stats, "{not json", 1.0)
        self.assertEqual(stats.n_content_frames, 0)


if __name__ == "__main__":
    unittest.main()
