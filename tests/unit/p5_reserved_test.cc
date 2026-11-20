// p5_reserved_test.cc — C-P5-01…03 默认值与值语义；证明预留不改变 v0 行为。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/p5_reserved.hpp"
#include "llmopt/sequence.hpp"

#include <type_traits>

using llmopt::SamplingParams;

TEST_CASE("预留块寻址默认指向本实例且未分配") {
    llmopt::p5::BlockRef ref;
    CHECK(ref.instance_id == llmopt::p5::InstanceId::Local);
    CHECK(ref.block_id == -1);
    CHECK(llmopt::p5::is_local(ref));
    CHECK(ref == llmopt::p5::BlockRef{});
    static_assert(!std::is_same_v<llmopt::p5::BlockRef, llmopt::BlockRef>,
                  "C-P5-01 与 v0 的 BlockRef 必须区分，避免同一名字被误用");
    static_assert(std::is_trivially_copyable_v<llmopt::p5::BlockRef>);
}

TEST_CASE("迁移快照默认是 v0 空状态") {
    llmopt::p5::MigratableRequestSnapshot snapshot;
    CHECK(snapshot.req_id.empty());
    CHECK(snapshot.prompt_ids.empty());
    CHECK(snapshot.block_table.empty());
    CHECK(snapshot.num_computed == 0);
    CHECK(snapshot.sampling.max_tokens == SamplingParams{}.max_tokens);
    CHECK(snapshot.sampling.seed == SamplingParams{}.seed);
}

TEST_CASE("迁移快照与重放轨迹保持值语义") {
    llmopt::p5::MigratableRequestSnapshot original;
    original.req_id = "req-1";
    original.prompt_ids = {1, 2, 3};
    original.block_table = {llmopt::p5::BlockRef{llmopt::p5::InstanceId::Local, 0},
                            llmopt::p5::BlockRef{llmopt::p5::InstanceId::Local, 1}};
    original.num_computed = 2;
    auto copy = original;
    copy.prompt_ids.push_back(4);
    copy.block_table[0].block_id = 9;
    CHECK(original.prompt_ids.size() == 3);
    CHECK(original.block_table[0].block_id == 0);
    CHECK(copy.num_computed == original.num_computed);

    llmopt::p5::ReplayTrace trace;
    CHECK(trace.seed == -1);
    CHECK(trace.accepted_tokens.empty());
    trace.accepted_tokens = original.prompt_ids;
    CHECK(trace.accepted_tokens.size() == 3);
}
