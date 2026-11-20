// cost_model_test.cc — 3-G3/3-G4 纯逻辑：victim 选择、recompute/swap 代价、状态迁移。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/scheduler.hpp"

#include <algorithm>
#include <memory>
#include <vector>

using namespace llmopt; // NOLINT：用例短名与契约同名

namespace {

// 构造可被选为 victim 的在跑序列：必须有块表且状态 Running。
std::shared_ptr<SequenceGroup> make_group(const std::string& id, std::int32_t prompt,
                                          std::int32_t output, std::int32_t blocks,
                                          std::int64_t admit_order, std::int64_t progress_ns) {
    auto group = std::make_shared<SequenceGroup>();
    group->req_id = id;
    group->seq.group_id = id;
    group->seq.prompt_len = prompt;
    group->seq.num_output = output;
    group->seq.num_computed = prompt + std::max<std::int32_t>(0, output - 1);
    group->seq.status = SeqStatus::Running;
    group->seq.block_table.resize(static_cast<std::size_t>(blocks));
    group->admit_order = admit_order;
    group->last_progress_ns = progress_ns;
    return group;
}

} // namespace

TEST_CASE("context_tokens 只计入已写 KV 的采样 token") {
    Sequence seq;
    seq.prompt_len = 10;
    seq.num_output = 0;
    CHECK(context_tokens(seq) == 10); // 尚无采样
    seq.num_output = 1;
    CHECK(context_tokens(seq) == 10); // 最后一个 token 未回填
    seq.num_output = 4;
    CHECK(context_tokens(seq) == 13);
}

TEST_CASE("fcfs_tail 选最晚准入，老化序列被豁免") {
    auto older = make_group("older", 8, 2, 2, 1, 100);
    auto newer = make_group("newer", 8, 2, 2, 2, 100);
    std::vector<SequenceGroup*> groups{older.get(), newer.get()};
    VictimPolicy policy;
    FairnessState fairness;
    CHECK(policy.select(groups, fairness) == std::vector<RequestId>{"newer"});

    fairness.aging_enabled = true;
    fairness.aging_threshold_ns = 100;
    fairness.now_ns = 1000;
    newer->last_progress_ns = 950; // 新序列仍活跃
    older->last_progress_ns = 100; // 超过阈值，豁免
    std::int32_t exempt = 0;
    CHECK(policy.select(groups, fairness, &exempt) == std::vector<RequestId>{"newer"});
    CHECK(exempt == 1);
}

TEST_CASE("weighted 策略按配置权重打分，默认全 0 保持 fcfs_tail") {
    auto long_seq = make_group("long", 32, 4, 5, 1, 100);
    auto short_seq = make_group("short", 4, 2, 2, 2, 100);
    std::vector<SequenceGroup*> groups{long_seq.get(), short_seq.get()};
    VictimPolicy policy;
    FairnessState fairness;
    VictimWeights weights;
    weights.blocks = 1.0; // 块越多分越高，优先释放大块
    policy.configure(weights);
    CHECK(policy.select(groups, fairness) == std::vector<RequestId>{"long"});
}

TEST_CASE("recompute 代价按 token 线性；未标定系数返回 NA") {
    const auto group = make_group("g", 8, 2, 2, 1, 100);
    CostModel model(0.5, 1024.0);
    CHECK(model.recompute_tokens(*group) == 9);
    CHECK(model.recompute_ms(9) == doctest::Approx(4.5));
    CHECK(model.recompute_ms(0) == -1.0);
    CostModel unset;
    CHECK(unset.recompute_ms(9) == -1.0);
}

TEST_CASE("swap 体积按块 × 容量 × 2 字节估算；无流水线时始终回落 recompute") {
    CostModel model(0.5, 2048.0);
    CHECK(model.swap_bytes(2, 16) == 64);
    CHECK(model.swap_bytes(0, 16) == 0);
    CHECK(model.swap_ms(2, 16) == doctest::Approx(64.0 / 2048.0));
    CHECK(model.swap_ms(0, 16) == -1.0);
    CHECK(model.choose(false, 1.0) == RecoveryPath::Recompute);
    CHECK(model.choose(true, 1.0) == RecoveryPath::Recompute);
}

TEST_CASE("状态迁移只允许四态图内的边") {
    CHECK(legal_transition(SeqStatus::Waiting, SeqStatus::Running));
    CHECK(legal_transition(SeqStatus::Running, SeqStatus::Swapped));
    CHECK(legal_transition(SeqStatus::Swapped, SeqStatus::Waiting));
    CHECK(legal_transition(SeqStatus::Running, SeqStatus::Finished));
    CHECK_FALSE(legal_transition(SeqStatus::Finished, SeqStatus::Running));
    CHECK_FALSE(legal_transition(SeqStatus::Waiting, SeqStatus::Finished));
    CHECK_FALSE(legal_transition(SeqStatus::Swapped, SeqStatus::Running));
}
