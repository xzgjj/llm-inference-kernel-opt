# 长 prompt 档（阶段 8，缩减规模）

## 为什么缩规模

长 prompt 档最初按 `P=1/2 × 30 请求 × 2 轮` 跑，单请求约 1.0 s prefill + 2.4 s decode；P=2 在 ngl=24 部分卸载下 CPU 带宽饱和，整档超过 6 分钟仍未完成，无法作为快速回归证据。按用户要求缩减为 **P=1、10 请求、1 轮、2 条预热**，保留“真实长 prefill + 真实 decode”两段，只放弃双并发与重复轮次。

缩减后单档约 35 s，进度由 `scripts/bench/watch_matrix.py` 每 10 s 写入 `progress.log`，报告出现即退出。

## 配置

| 项 | 值 |
|---|---|
| 模型 | `Qwen3-8B-Q4_K_M.gguf` |
| 并发 | P=1 |
| 请求 / 轮次 | 10 / 1（另 2 条预热不计入报告） |
| ctx / max_tokens | 1280 / 32 |
| prompt 档 | 1400 非空白字符，实测 **522 token/请求** |
| 前缀缓存 | **关闭**（`--disable-prefix-cache`，强制每请求完整 prefill） |
| 上卡 | `ngl=24`，`ngl=24/37` |
| chunk | `max_num_batched_tokens=1024`，`chunk_size=128` |

## 结果

| 指标 | 实测 |
|---|---|
| 成功 / 总请求 | 10 / 10 |
| TTFT p50 / p95 | **1045.205 / 1107.186 ms** |
| ITL p99 | **173.960 ms** |
| 单路 decode p50 | **13.58 tok/s**（ITL p50 73.6 ms，含预热） |
| 总 token 吞吐 | **155.725 tok/s**（prefill 主导，不能当 decode 能力） |
| 进程显存 `vram_peak_mib` | **3459.33 MiB**（`buffers`，三项齐全） |
| 全卡峰值 | 6610.0 MiB |
| `kv_used_peak` | 35 块 |
| chunk 步数 | 48 |
| 前缀命中 token | 0（缓存已关） |
| 实算 prefill token | 5220（10 请求 × 522） |

## 结论

1. **长 prompt 的真实成本在 prefill，不在 decode**：TTFT 1.05 s、每请求 522 token；decode 仍只有 13.6 tok/s。`total_tok_s=155.7` 是“每秒处理的 prompt+生成 token”，把它当生成速度会高估 11 倍。
2. **关闭前缀缓存是必要控制**：不关时同一长前缀被整块命中，`prefix_hit_tokens` 可达 5184/轮，实际只算十几 token，长 prompt 档会名不副实。
3. **P=1 没有 KV 压力**：`kv_used_peak=35/512`，抢占为 0；长 prompt 与 KV 压力是两个独立实验，压力档仍用 P=16/ctx512。
4. 缩减规模后证据仍覆盖“完整 prefill + decode + 显存 + chunk 计数”，只牺牲并发与两轮偏差；偏差列因此为 `na`。

donor 口径：该轮配置省略 `donor_slots`，服务端按默认 donor=4 解析；但前缀缓存已关，`donor_slots_for` 返回 0，`n_seq_max=1`（`server-p1.log` 实测），donor 槽位没有参与。矩阵代码已改为始终显式写 `--donor-slots`，避免 manifest 与实跑口径漂移。

KV 类型：该轮配置未写 `kv_cache_type`，实际按服务默认 `f16` 运行；配置已显式补 `f16`，旧 manifest 早于该键。

## 复现

```bash
cd /mnt/d/c++_myproject/llm-inference-kernel-opt
python3 scripts/bench/matrix_runner.py \
  --config scripts/bench/matrix/phase8b_long.json \
  --out bench/results/phase8b_long
# 进度（另开终端）
python3 scripts/bench/watch_matrix.py --out bench/results/phase8b_long \
  --log bench/results/phase8b_long/progress.log --interval 10 --until-report
```
