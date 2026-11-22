# Phase 13 四组消融矩阵（D2-03）

固定条件：WSL Ubuntu 26.04，RTX 4060 Laptop 8 GB，Qwen3-8B-Q3_K_M，`ngl 37` 全驻留 + `q8_0` KV，`ctx 1024`，`max_num_batched_tokens 1024`，`queue_capacity 32`，temperature 0，每档 30 请求 × 5 轮。原始产物在四个子目录：`matrix_report.csv` / `deviation.csv` / `step_summary.csv` / `repro_manifest.json` / `metrics_p*_r*.prom` / `plots/`。

## static 与 mixed（continuous）对照

| P | static TTFT p50 | continuous TTFT p50 | static 吞吐 | continuous 吞吐 | ITL p99（static / continuous） |
| --- | --- | --- | --- | --- | --- |
| 1 | 29.4 ms | 29.8 ms | 62.0 tok/s | 61.0 tok/s | 23.2 / 23.3 ms |
| 2 | 744.5 ms | 70.9 ms | 62.2 tok/s | 51.5 tok/s | 23.4 / 51.7 ms |
| 4 | 2160.3 ms | 168.3 ms | 56.0 tok/s | 61.2 tok/s | 70.2 / 112.0 ms |
| 8 | 6970.0 ms | 307.2 ms | 57.1 tok/s | 62.4 tok/s | 142.1 / 208.9 ms |

**结论（定上限，不翻案）**：

1. 补位（continuous）的净收益在**首 token 等待**上：P8 把 TTFT p50 从 6970 ms 压到 307 ms（22.7×），P4 从 2160 ms 压到 168 ms。
2. 吞吐收益远不到 1.3×：P8 仅 57.1 → 62.4 tok/s（+9.3%），未触发 SPEC 的翻案判据，因此**继续保留“全驻留档吞吐被带宽封顶”的结论**。
3. 代价是 ITL p99：P8 continuous 208.9 ms 已越过 200 ms SLO，故**交互式容量上限定为 P=4**（ITL 112 ms、TTFT 168 ms，均在目标内）。P=8 只适合离线批处理。

## prefix on / off

`prefix_on` 首次运行暴露真实缺陷：第二条及之后的请求大量 `empty_stream`，服务端报 `inconsistent sequence positions (last=127, start=112)`，最终 llama 侧 `GGML_ASSERT(is_full && "seq_cp() is only supported for full KV buffers")` 崩溃。修复见下节；修复前的产物保留在 `perf-prefix-on-pre-fix/`。

修复后重跑（同一台机、同模型、5 轮 × 30 请求）：

| P | prefix off TTFT p50 | prefix on TTFT p50 | off 吞吐 | on 吞吐 | off / on ITL p99 | on 分片率 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 102.1 ms | 34.4 ms | 242.4 tok/s | 265.5 tok/s | 23.2 / 22.7 ms | 0.133 |
| 4 | 411.1 ms | 349.1 ms | 290.4 tok/s | 281.0 tok/s | 73.3 / 71.8 ms | 0.167 |
| 8 | 792.3 ms | 654.5 ms | 317.1 tok/s | 332.6 tok/s | 119.1 / 134.1 ms | 0.174 |

**结论**：前缀复用的主收益是**首 token 延迟**（P1 3×、P8 −17%），吞吐 +5%~9.5%，P4 略降；代价是 P8 的 ITL p99 +15 ms（119 → 134 ms，仍在 200 ms 内）。共享 384 token 前缀的场景值得开；不等长负载下收益会退化，不做更宽结论。`insufficient` 标记来自矩阵自身的样本阈值判定（每档 30 请求），这里仍按 5 轮均值引用，并保留 `deviation.csv` 供复核。

**修复**：`src/runner_llama.cpp` 里 `seq_cp` 前先把 donor 的 KV 裁到「块表给出的可复用前缀长度」：先用 `llama_memory_seq_rm(donor, prefix_tokens, -1)` 删掉多余尾部，再整段 `seq_cp`；若 donor 实际长度短于 `prefix_tokens` 则直接放弃命中。根因是 donor 逻辑上 `keep_tokens` 截断、物理上仍留着更长位置，块表命中长度与 donor 实际长度不一致时就会复制出重叠位置。回归用例：`tests/e2e/nightly_real_backend.py` 的 `prefix_reuse` 检查（两条同前缀长请求连发，第二条必须 200）。

## 复现

```bash
# 四组顺序：static → mixed → prefix_on → prefix_off
LLMOPT_SERVER=/root/build-llmopt/llmopt_server \
LLMOPT_MODEL=/mnt/d/ai_model/models/qwen3-8b/Qwen3-8B-Q3_K_M.gguf \
bash scripts/bench/run_phase13_matrices.sh /root/work/llmopt
```

单组重跑（修完缺陷后只补 prefix 两组）：`bash scripts/bench/rerun_prefix13.sh /root/work/llmopt`。
