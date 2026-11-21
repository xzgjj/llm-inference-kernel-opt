# 短 prompt 矩阵（真实 token 口径）

配置：`Qwen3-8B-Q4_K_M`，`ngl=24`、`threads=0`、ctx=1024、P=1/2/4、short≤128 字符、30 请求 × 32 token × 2 轮。

| P | TTFT p50 | ITL p99 | 总 token 吞吐 | decode p50 | `vram_peak_mib` | 偏差 |
|---|---|---|---|---|---|---|
| 1 | 208.366 ms | 131.897 ms | 26.266 tok/s | 12.83 tok/s | 3804.52 | ok（2.0%） |
| 2 | 445.847 ms | 324.689 ms | 29.659 tok/s | 6.61 tok/s | 3896.58 | high（8.9%） |
| 4 | 913.761 ms | 657.152 ms | 25.656 tok/s | 3.10 tok/s | 4080.33 | high（11.5%） |

`tok_accounting=prompt_metrics`：prompt 用 `llmopt_prefill_tokens_total + llmopt_prefix_hit_tokens_total`，不再用字符估算。旧 54.107/57.842/62.076 tok/s 已作废；P=4 在部分卸载下没有吞吐增益。

`vram_peak_scope=buffers`（CUDA0 model+KV+compute 三项齐全）；`vram_device_peak_mib` 7171/7075/7347 是全卡口径，含桌面占用，只作扰动观察。

**donor 口径说明**：该轮矩阵配置省略 `donor_slots`，`server_argv` 未写 `--donor-slots`；但服务端 `server-p1.log` 的 `n_seq_max=5`（P=1 + 4）证明实际就是默认 donor=4。代码已改为始终显式写 donor，后续 manifest 不再有这一歧义；本轮结果不受影响。

复现：

```bash
python3 scripts/bench/matrix_runner.py --config scripts/bench/matrix/phase8b_short.json \
  --out bench/results/phase8b_vram
```
