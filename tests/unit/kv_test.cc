// kv_test.cc — C-06 逻辑块表：取整、复用、不足不改动序列。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/kv.hpp"

using namespace llmopt; // NOLINT：用例短名与契约同名

namespace {

// 就地填一个只带 id 的空序列；Sequence 含 atomic，禁止拷贝/移动。
void init_sequence(Sequence& seq, std::int32_t id) {
    seq.id = id;
    seq.group_id = "req-kv";
    seq.prompt_len = 0;
}

} // namespace

TEST_CASE("按 ceil(n_tokens/block) 分配并在释放后归还") {
    KVBlockManager kv(KVConfig{16, 4});
    Sequence seq;
    init_sequence(seq, 1);
    CHECK(kv.num_free_blocks() == 4);
    REQUIRE(kv.acquire(seq, 20).has_value());
    CHECK(seq.block_table.size() == 2);
    CHECK(kv.num_free_blocks() == 2);
    REQUIRE(kv.acquire(seq, 40).has_value());
    CHECK(seq.block_table.size() == 3);
    CHECK(kv.num_free_blocks() == 1);
    kv.free(seq);
    CHECK(seq.block_table.empty());
    CHECK(kv.num_free_blocks() == 4);
}

TEST_CASE("块不足时返回 nullopt 且不产生半分配") {
    KVBlockManager kv(KVConfig{16, 2});
    Sequence seq;
    init_sequence(seq, 2);
    REQUIRE(kv.acquire(seq, 16).has_value());
    CHECK(seq.block_table.size() == 1);
    CHECK_FALSE(kv.acquire(seq, 100).has_value());
    CHECK(seq.block_table.size() == 1);
    CHECK(kv.num_free_blocks() == 1);
    kv.free(seq);
}

TEST_CASE("零 token 不占块，lookahead 计入需求") {
    KVBlockManager kv(KVConfig{8, 3});
    Sequence seq;
    init_sequence(seq, 3);
    const auto none = kv.acquire(seq, 0);
    REQUIRE(none.has_value());
    CHECK(none->empty());
    REQUIRE(kv.acquire(seq, 1, 8).has_value());
    CHECK(seq.block_table.size() == 2);
}

TEST_CASE("容量换算按 block_tokens × num_blocks") {
    KVBlockManager kv(KVConfig{16, 10});
    CHECK(kv.num_total_blocks() == 10);
    CHECK(kv.block_capacity_tokens() == 160);
}
