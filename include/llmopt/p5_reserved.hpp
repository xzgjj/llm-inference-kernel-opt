// p5_reserved.hpp — SPEC-5 的 C-P5-01…03 预留值对象；默认值等价 v0，且不得被 src 引用。
#ifndef LLMOPT_P5_RESERVED_HPP
#define LLMOPT_P5_RESERVED_HPP

#include "llmopt/common.hpp"

#include <cstdint>
#include <type_traits>
#include <vector>

namespace llmopt::p5 {

// C-P5-01：块全局唯一寻址。本期只有 Local，远端实例值不存在，跨实例块不可本地解引用。
enum class InstanceId : std::uint8_t { Local = 0 };

using BlockId = std::int32_t;

struct BlockRef {
    InstanceId instance_id = InstanceId::Local; // v0 恒 Local
    BlockId block_id = -1;                      // 段内 append-only ID，-1 表示未分配

    friend bool operator==(const BlockRef&, const BlockRef&) = default;
};

// C-P5-02：迁移快照是值对象，迁移不携带裸指针（INV-01）；默认构造即 v0 空状态。
struct MigratableRequestSnapshot {
    RequestId req_id;
    std::vector<std::int32_t> prompt_ids;
    std::vector<BlockRef> block_table;
    std::int32_t num_computed = 0;
    SamplingParams sampling;
};

// C-P5-03：采样可重放；seed=-1 与 SamplingParams 的“服务端派生”语义一致，accepted 链本期等于
// output。
struct ReplayTrace {
    std::int64_t seed = -1;
    std::vector<std::int32_t> accepted_tokens;
};

static_assert(std::is_trivially_copyable_v<BlockRef>,
              "C-P5-01 必须是可平凡复制的寻址值，跨线程只传值");

[[nodiscard]] constexpr bool is_local(const BlockRef& ref) noexcept {
    return ref.instance_id == InstanceId::Local;
}

} // namespace llmopt::p5

#endif // LLMOPT_P5_RESERVED_HPP
