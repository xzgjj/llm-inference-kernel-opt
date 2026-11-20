// chunk_planner_test.cc — SPEC-3 3-G1：切块宽度、预算截断与末块标记的纯逻辑用例。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/scheduler.hpp"

using namespace llmopt; // NOLINT：用例与被测类型同名

TEST_CASE("chunk 128 在 320 token prompt 上按 128/128/64 收尾") {
    ChunkPlanner planner(128);
    const ChunkSlice first = planner.next(320, 0, 256);
    CHECK(first.n_tokens == 128);
    CHECK_FALSE(first.is_last);
    const ChunkSlice second = planner.next(320, 128, 256);
    CHECK(second.n_tokens == 128);
    CHECK_FALSE(second.is_last);
    const ChunkSlice third = planner.next(320, 256, 256);
    CHECK(third.n_tokens == 64);
    CHECK(third.is_last);
}

TEST_CASE("剩余预算不足 chunk 宽时按预算截断且不误判末块") {
    ChunkPlanner planner(128);
    const ChunkSlice slice = planner.next(200, 0, 10);
    CHECK(slice.n_tokens == 10);
    CHECK_FALSE(slice.is_last);
    // 截断后继续推进，预算恢复时仍按 128 切，直到剩余不足。
    const ChunkSlice next = planner.next(200, 10, 128);
    CHECK(next.n_tokens == 128);
    CHECK_FALSE(next.is_last);
    const ChunkSlice last = planner.next(200, 138, 128);
    CHECK(last.n_tokens == 62);
    CHECK(last.is_last);
}

TEST_CASE("prompt 已算完或预算为零时不产出 chunk") {
    ChunkPlanner planner(64);
    CHECK(planner.next(32, 32, 128).n_tokens == 0);
    CHECK(planner.next(32, 40, 128).n_tokens == 0);
    CHECK(planner.next(32, 0, 0).n_tokens == 0);
    CHECK(planner.next(32, 0, -1).n_tokens == 0);
}

TEST_CASE("短 prompt 一步即末块，非法宽度收敛为 1") {
    ChunkPlanner short_planner(128);
    const ChunkSlice one = short_planner.next(6, 0, 256);
    CHECK(one.n_tokens == 6);
    CHECK(one.is_last);
    ChunkPlanner bad(0);
    CHECK(bad.chunk_size() == 1);
    CHECK(bad.next(4, 0, 4).n_tokens == 1);
}

TEST_CASE("批次构成快照按 phase 区分 prefill 与 decode") {
    Batch batch;
    SequenceStep prefill;
    prefill.phase = Phase::PrefillChunk;
    prefill.n_tokens = 7;
    SequenceStep last;
    last.phase = Phase::PrefillLast;
    last.n_tokens = 3;
    SequenceStep decode;
    decode.phase = Phase::Decode;
    decode.n_tokens = 1;
    batch.steps = {prefill, last, decode};
    batch.total_tokens = 11;
    const BatchComposition composition = batch_composition(batch);
    CHECK(composition.prefill_tokens == 10);
    CHECK(composition.decode_tokens == 1);
    CHECK(composition.prefill_seqs == 2);
    CHECK(composition.decode_seqs == 1);
    CHECK(composition.steps() == 3);
}
