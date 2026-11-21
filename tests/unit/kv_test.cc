// kv_test.cc — C-06/C-P2-13 契约：分配、引用计数、watermark、前缀命中与缓存淘汰。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/kv.hpp"

using namespace llmopt; // NOLINT：用例短名与契约同名

namespace {

// 构造只带必要字段的序列；prompt_token_ids 决定块哈希链。
void init_sequence(Sequence& seq, std::int32_t id, std::int32_t prompt_len) {
    seq.id = id;
    seq.backend_id = "req-" + std::to_string(id);
    seq.prompt_len = prompt_len;
    seq.prompt_token_ids.assign(static_cast<std::size_t>(prompt_len), 0);
    for (std::int32_t index = 0; index < prompt_len; ++index) {
        seq.prompt_token_ids[static_cast<std::size_t>(index)] = 100 + index;
    }
}

} // namespace

TEST_CASE("按 ceil(n_tokens/block) 分配并在释放后归还") {
    KVBlockManager kv(KVConfig{16, 4});
    Sequence seq;
    init_sequence(seq, 1, 0);
    CHECK(kv.num_free_blocks() == 4);
    REQUIRE(kv.acquire(seq, 20).ok());
    CHECK(seq.block_table.size() == 2);
    CHECK(kv.num_free_blocks() == 2);
    REQUIRE(kv.acquire(seq, 40).ok());
    CHECK(seq.block_table.size() == 3);
    CHECK(kv.num_free_blocks() == 1);
    kv.free(seq);
    CHECK(seq.block_table.empty());
    CHECK(kv.num_free_blocks() == 4);
    std::string why;
    CHECK(kv.check_invariants(why));
}

TEST_CASE("块不足时返回 NoFreeBlock 且不产生半分配") {
    KVBlockManager kv(KVConfig{16, 3});
    Sequence first;
    init_sequence(first, 2, 0);
    REQUIRE(kv.acquire(first, 32).ok());
    Sequence second;
    init_sequence(second, 3, 0);
    REQUIRE(kv.acquire(second, 16).ok());
    CHECK(second.block_table.size() == 1);
    const AllocResult result = kv.acquire(second, 32);
    CHECK_FALSE(result.ok());
    CHECK(result.reason == AllocFailReason::NoFreeBlock);
    CHECK(second.block_table.size() == 1);
    kv.free(first);
    kv.free(second);
    std::string why;
    CHECK(kv.check_invariants(why));
}

TEST_CASE("超过块池容量返回 ContextOverflow") {
    KVBlockManager kv(KVConfig{16, 2});
    Sequence seq;
    init_sequence(seq, 4, 0);
    const AllocResult overflow = kv.acquire(seq, 33);
    CHECK(overflow.reason == AllocFailReason::ContextOverflow);
}

TEST_CASE("零 token 不占块，lookahead 计入需求") {
    KVBlockManager kv(KVConfig{8, 3});
    Sequence seq;
    init_sequence(seq, 3, 0);
    CHECK(kv.acquire(seq, 0).ok());
    CHECK(seq.block_table.empty());
    REQUIRE(kv.acquire(seq, 1, 8).ok());
    CHECK(seq.block_table.size() == 2);
    kv.free(seq);
}

TEST_CASE("watermark 不足时拒绝准入但 decode 增量不预留") {
    KVBlockManager kv(KVConfig{16, 4, 0.50, false, 0});
    CHECK(kv.watermark_blocks() == 3);
    Sequence seq;
    init_sequence(seq, 4, 0);
    REQUIRE(kv.acquire(seq, 1, 0, true).ok());
    CHECK(kv.num_free_blocks() == 3);
    const AllocResult blocked = kv.acquire(seq, 17, 0, true);
    CHECK_FALSE(blocked.ok());
    CHECK(blocked.reason == AllocFailReason::BelowWatermark);
    REQUIRE(kv.acquire(seq, 17, 0, false).ok());
    CHECK(seq.block_table.size() == 2);
    kv.free(seq);
}

TEST_CASE("前缀命中：第二次请求共享满块且只补差异块") {
    KVBlockManager kv(KVConfig{4, 8, 0.0, true, 2});
    Sequence first;
    init_sequence(first, 5, 9);
    REQUIRE(kv.acquire(first, 12, 0, true).ok());
    kv.mark_written(first, 9);
    const std::int32_t kept = kv.retain(first);
    CHECK(kept == 8);
    CHECK(first.block_table.empty());
    CHECK(kv.usage().cached_blocks == 2);

    Sequence second;
    init_sequence(second, 6, 9);
    const PrefixHit hit = kv.lookup_prefix(second);
    REQUIRE(hit.hit_tokens == 8);
    REQUIRE(hit.blocks.size() == 2);
    CHECK(kv.attach_prefix(second, hit) == 8);
    second.num_computed = 8;
    REQUIRE(kv.acquire(second, 12, 0, true).ok());
    CHECK(second.block_table.size() == 3);
    CHECK(kv.usage().shared_blocks >= 2);

    kv.free(second);
    CHECK(second.block_table.empty());
    CHECK(kv.usage().cached_blocks == 2);
    std::string why;
    CHECK(kv.check_invariants(why));

    const std::vector<RequestId> dropped = kv.flush_cache();
    CHECK(dropped.size() == 1);
    CHECK(kv.num_free_blocks() == kv.num_total_blocks());
    CHECK(kv.check_invariants(why));
}

TEST_CASE("未写完的块不能被前缀命中") {
    KVBlockManager kv(KVConfig{4, 8, 0.0, true, 2});
    Sequence first;
    init_sequence(first, 7, 8);
    REQUIRE(kv.acquire(first, 8, 0, true).ok());
    const PrefixHit pending = kv.lookup_prefix(first);
    CHECK(pending.hit_tokens == 0);
    kv.mark_written(first, 3);
    const PrefixHit partial = kv.lookup_prefix(first);
    CHECK(partial.hit_tokens == 0);
    kv.mark_written(first, 4);
    const PrefixHit ready = kv.lookup_prefix(first);
    CHECK(ready.hit_tokens == 4);
    kv.free(first);
}

TEST_CASE("缓存占满时按 donor 淘汰并给出待释放序列") {
    KVBlockManager kv(KVConfig{4, 4, 0.0, true, 1});
    Sequence first;
    init_sequence(first, 8, 16);
    REQUIRE(kv.acquire(first, 16, 0, true).ok());
    kv.mark_written(first, 16);
    REQUIRE(kv.retain(first) == 16);
    CHECK(kv.num_free_blocks() == 0);

    Sequence second;
    init_sequence(second, 9, 4);
    const AllocResult result = kv.acquire(second, 4, 0, true);
    REQUIRE(result.ok());
    const std::vector<RequestId> dropped = kv.take_dropped_donors();
    REQUIRE(dropped.size() == 1);
    CHECK(dropped.front() == "req-8");
    CHECK(kv.usage().cached_blocks == 0);
    kv.free(second);
    std::string why;
    CHECK(kv.check_invariants(why));
}

// ASan 抓到过 heap-use-after-free：donor_order_ 里被 erase 的元素正是传入的 donor 引用，
// 再 push_back(donor) 就是读已释放内存。短 id 走 SSO 感知不到，故用超过 15 字节的 id 锁住该路径。
TEST_CASE("长 donor id 淘汰后仍安全写入待释放列表") {
    KVBlockManager kv(KVConfig{4, 4, 0.0, true, 1});
    Sequence first;
    init_sequence(first, 8, 16);
    first.backend_id = "req-0123456789abcdef";
    REQUIRE(kv.acquire(first, 16, 0, true).ok());
    kv.mark_written(first, 16);
    REQUIRE(kv.retain(first) == 16);
    CHECK(kv.num_free_blocks() == 0);

    Sequence second;
    init_sequence(second, 9, 4);
    REQUIRE(kv.acquire(second, 4, 0, true).ok());
    const std::vector<RequestId> dropped = kv.take_dropped_donors();
    REQUIRE(dropped.size() == 1);
    CHECK(dropped.front() == "req-0123456789abcdef");
    kv.free(second);
    std::string why;
    CHECK(kv.check_invariants(why));
}

TEST_CASE("共享块在 ref_cnt>1 时不被回收，最后一个持有者释放后才空闲") {
    KVBlockManager kv(KVConfig{4, 4, 0.0, true, 2});
    Sequence first;
    init_sequence(first, 10, 12);
    REQUIRE(kv.acquire(first, 12, 0, true).ok());
    kv.mark_written(first, 12);
    Sequence second;
    init_sequence(second, 11, 12);
    const PrefixHit hit = kv.lookup_prefix(second);
    REQUIRE(hit.hit_tokens == 8);
    CHECK(kv.attach_prefix(second, hit) == 8);
    REQUIRE(kv.acquire(second, 12, 0, true).ok());
    CHECK(kv.usage().shared_blocks == 2);

    kv.free(first);
    CHECK(kv.usage().shared_blocks == 0);
    CHECK(kv.num_free_blocks() == 1);
    kv.free(second);
    CHECK(kv.usage().cached_blocks == 0);
    CHECK(kv.num_free_blocks() == kv.num_total_blocks());
    std::string why;
    CHECK(kv.check_invariants(why));
}

TEST_CASE("容量换算按 block_tokens × num_blocks，越界请求返回 ContextOverflow") {
    KVBlockManager kv(KVConfig{16, 10});
    CHECK(kv.num_total_blocks() == 10);
    CHECK(kv.block_capacity_tokens() == 160);
    Sequence seq;
    init_sequence(seq, 12, 0);
    const AllocResult overflow = kv.acquire(seq, 161);
    CHECK(overflow.reason == AllocFailReason::ContextOverflow);
}
