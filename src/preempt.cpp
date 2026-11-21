// preempt.cpp — 3-G3/3-G4：victim 选择、代价模型、抢占试算与 trace 落盘。
#include "llmopt/scheduler.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>

namespace llmopt {
namespace {

const char* trigger_name(PreemptTrigger trigger) {
    return trigger == PreemptTrigger::ChunkPlan ? "chunk_plan" : "admission";
}

const char* path_name(RecoveryPath path) {
    return path == RecoveryPath::Swap ? "swap" : "recompute";
}

} // namespace

void VictimPolicy::configure(VictimWeights weights) noexcept {
    weights_ = weights;
}

std::vector<RequestId> VictimPolicy::select(const std::vector<SequenceGroup*>& groups,
                                            const FairnessState& fairness,
                                            std::int32_t* aging_exempt) const {
    SequenceGroup* best = nullptr;
    double best_score = 0.0;
    for (SequenceGroup* group : groups) {
        if (group == nullptr || group->seq.status != SeqStatus::Running ||
            group->seq.block_table.empty()) {
            continue;
        }
        // 老化豁免：等待/无进展超过阈值的请求不再被选为 victim（默认关闭）。
        if (fairness.aging_enabled && group->last_progress_ns > 0 &&
            fairness.now_ns - group->last_progress_ns >= fairness.aging_threshold_ns) {
            if (aging_exempt != nullptr) {
                ++*aging_exempt;
            }
            continue;
        }
        const bool weighted = weights_.priority != 0.0 || weights_.progress != 0.0 ||
                              weights_.blocks != 0.0 || weights_.remaining != 0.0;
        double score = 0.0;
        if (weighted) {
            const std::int32_t remaining =
                    std::max<std::int32_t>(0, context_tokens(group->seq) - group->seq.num_computed);
            score = weights_.priority * static_cast<double>(group->admit_order) +
                    weights_.progress * static_cast<double>(group->seq.num_computed) +
                    weights_.blocks * static_cast<double>(group->seq.block_table.size()) +
                    weights_.remaining * static_cast<double>(remaining);
        } else {
            // fcfs_tail：最晚准入者优先；无权重时用 admit_order 决定。
            score = static_cast<double>(group->admit_order);
        }
        if (best == nullptr || score > best_score) {
            best = group;
            best_score = score;
        }
    }
    std::vector<BackendId> out;
    if (best != nullptr) {
        out.push_back(best->backend_id);
    }
    return out;
}

CostModel::CostModel(double recompute_ms_per_token, double swap_bytes_per_ms) noexcept
    : recompute_ms_per_token_(recompute_ms_per_token), swap_bytes_per_ms_(swap_bytes_per_ms) {}

std::int32_t CostModel::recompute_tokens(const SequenceGroup& group) const noexcept {
    return context_tokens(group.seq);
}

double CostModel::recompute_ms(std::int32_t tokens) const noexcept {
    if (recompute_ms_per_token_ <= 0.0 || tokens <= 0) {
        return -1.0;
    }
    return static_cast<double>(tokens) * recompute_ms_per_token_;
}

std::int64_t CostModel::swap_bytes(std::int32_t blocks, std::int32_t cap_tokens) const noexcept {
    if (blocks <= 0 || cap_tokens <= 0) {
        return 0;
    }
    // 仅用于量级估算：按每 token 2 字节（fp16）的 KV 体积下界，不含层数放大。
    return static_cast<std::int64_t>(blocks) * cap_tokens * 2;
}

double CostModel::swap_ms(std::int32_t blocks, std::int32_t cap_tokens) const noexcept {
    if (swap_bytes_per_ms_ <= 0.0) {
        return -1.0;
    }
    const std::int64_t bytes = swap_bytes(blocks, cap_tokens);
    return bytes <= 0 ? -1.0 : static_cast<double>(bytes) / swap_bytes_per_ms_;
}

RecoveryPath CostModel::choose(bool swap_enabled, double margin) const noexcept {
    (void)margin;
    if (!swap_enabled) {
        return RecoveryPath::Recompute;
    }
    return RecoveryPath::Recompute; // v0 未接换出流水线，始终回落 recompute。
}

PreemptTrace::PreemptTrace(const std::string& path) {
    if (!path.empty()) {
        file_.open(path, std::ios::out | std::ios::trunc);
    }
}

void PreemptTrace::record(const PreemptEvent& event) {
    if (!file_.is_open()) {
        return;
    }
    if (!header_written_) {
        file_ << "ts_ns,trigger,victim,free_before,need_blocks,blocks_released,recompute_tokens,"
                 "path,cost_ms,cap_error\n";
        header_written_ = true;
    }
    file_ << event.ts_ns << ',' << trigger_name(event.trigger) << ',' << event.victim << ','
          << event.free_before << ',' << event.need_blocks << ',' << event.blocks_released << ','
          << event.recompute_tokens << ',' << path_name(event.path) << ',';
    if (event.cost_ms < 0.0) {
        file_ << "NA";
    } else {
        file_ << std::fixed << std::setprecision(3) << event.cost_ms;
    }
    file_ << ',' << (event.cap_error ? 1 : 0) << '\n';
}

PreemptDecision try_preempt(const std::vector<SequenceGroup*>& groups, std::int32_t need_blocks,
                            KVBlockManager& kv, const VictimPolicy& policy,
                            const FairnessState& fairness) {
    PreemptDecision decision;
    // 先做无副作用可行性预判：禁止先清空 running 再发现仍然不够。
    std::int32_t max_releasable = kv.num_free_blocks();
    for (SequenceGroup* group : groups) {
        if (group == nullptr || group->seq.status != SeqStatus::Running ||
            group->seq.block_table.empty()) {
            continue;
        }
        if (fairness.aging_enabled && group->last_progress_ns > 0 &&
            fairness.now_ns - group->last_progress_ns >= fairness.aging_threshold_ns) {
            continue;
        }
        max_releasable += kv.releasable_blocks(group->seq);
    }
    if (max_releasable < need_blocks) {
        decision.retry_ok = false;
        return decision;
    }
    while (kv.num_free_blocks() < need_blocks) {
        const std::vector<BackendId> selected =
                policy.select(groups, fairness, &decision.aging_exempt);
        if (selected.empty()) {
            break;
        }
        SequenceGroup* victim = nullptr;
        for (SequenceGroup* group : groups) {
            if (group != nullptr && group->backend_id == selected.front()) {
                victim = group;
                break;
            }
        }
        if (victim == nullptr || victim->seq.block_table.empty()) {
            break;
        }
        const std::int32_t free_before = kv.num_free_blocks();
        kv.free(victim->seq);
        const std::int32_t released = kv.num_free_blocks() - free_before;
        decision.victims.push_back(victim->backend_id);
        decision.released_per_victim.push_back(released);
        decision.blocks_released += released;
    }
    decision.retry_ok = kv.num_free_blocks() >= need_blocks;
    return decision;
}

} // namespace llmopt
