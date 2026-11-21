# S3 性能对照（阶段 12，A 档全驻留）

**结论：在本机 A 档配置下，continuous batching 的吞吐收益被证伪（P8/P1 = 1.05×，判据要求 ≥1.3×），代价是 ITL p99 放大 7.4×。瓶颈不在控制面，而在"单执行线程 + GPU 显存带宽"这一后端上限。**

**这条结论只覆盖：** 等长 32-token 短解码、单档 prompt、P=1/2/4/8、全驻留、单执行线程。它证明"并发从 1 拉到 8 不再涨吞吐"，不能推出"continuous batching 本身没有收益"——等长输出抹掉了"短请求先完成、槽位立刻复用"这一项，且没有静态批基线，1.05× 无法精确归因到补位机制。

## 配置

| 项 | 值 |
|---|---|
| 模型 | Qwen3-8B-Q3_K_M，sha256 `c2c61b55…502c3` |
| GPU | RTX 4060 Laptop 8 GB（WSL Ubuntu 26.04，CUDA 13.5，llama.cpp `b12557` CUDA 构建） |
| 层分布 | `ngl=37`，37/37 层在 `CUDA0`；`token_embd` 留 CPU_Host（llama.cpp 限制） |
| KV | `q8_0`；ctx 1024；`max-num-batched-tokens` 1024 |
| 采样 | temperature 0、top_k 1 |
| 负载 | 短档 prompt ≤64 token，**等长** `max_tokens=32`，每档 30 请求 × **5 轮**，warmup 3 |
| 并发 | P = 1 / 2 / 4 / 8 |
| 显存 | model 3672 MiB + KV 382→918 MiB + compute 330 MiB（P8 峰值 4920 MiB buffers，设备 7242 MiB） |

## 结果（`matrix_report.csv`）

| P | TTFT p50 (ms) | ITL p99 (ms) | 吞吐 (tok/s) | 相对 P1 吞吐 | sched_cpu_ratio | step p50 / p99 (µs) | 轮间偏差 |
|---|---|---|---|---|---|---|---|
| 1 | 28.5 | 23.3 | 77.1 | 1.00× | 0.0002 | 4 / 11 | ok |
| 2 | 82.6 | 67.7 | 62.0 | 0.80× | 0.0001 | 5 / 21 | high |
| 4 | 207.1 | 108.1 | 78.2 | 1.01× | 0.0001 | 5 / 31 | high |
| 8 | 320.6 | 171.3 | 80.7 | **1.05×** | 0.0001 | 8 / 56 | high |

判据（SPEC-DEBUG1 §5.4）：
- 证明收益：P≥2 吞吐 ≥1.3×P1 且 ITL p99 ≤3×P1 → **不成立**（最好 1.05×，P8 ITL p99 已 7.4×）。
- 证伪收益：给出瓶颈归因与 CB 上限位置 → **成立**，见下。

## 归因（为什么全驻留后 batching 不涨）

1. **控制面不是瓶颈**：`sched_cpu_ratio` 全程 0.0001–0.0002（0.01–0.02% CPU），调度步 p50 4–8 µs、p99 ≤56 µs。即使 P8，调度开销相对 25 ms/步的 GPU 计算可忽略。
2. **单执行线程串行化**：`pending_step_` 结构决定"调度 → 提交 → 等完成 → 下一步"零重叠；batch 内 token 数增加只让每一步变长，不改变步的串行本质。P8 每步约 8 token/路 × 8 路 ≈ 64 token，步时长随之线性上升，吞吐因此停在 ~80 tok/s。
3. **显存带宽封顶**：8B Q3_K_M 每 token 需读约 3.7 GB 权重；77–81 tok/s 对应约 290 GB/s 有效带宽，已接近该卡在 WSL 下的可用带宽上限。batch 只分摊权重读取的固定开销，不能突破带宽本身。
4. **上限位置**：吞吐在 P=4 已到平台（78.2 → 80.7），再往上只增加排队与尾延迟；P=8 的 TTFT p50 320 ms、ITL p99 171 ms 是纯代价。

## 与旧矩阵的关系

旧矩阵（`ngl 24`、13/37 层留 CPU）给出的 "P=4 1.61×" 与 "P8 1.5×" 是 **CPU-offload 天花板**下的相对改善：batch 让 CPU 侧权重读取被多路分摊。全驻留后该天花板消失，batching 的边际收益随之消失。旧表在 README 中标注作废，只引用本表。

## 复现

```bash
# WSL，CUDA 版 llama.cpp 与 llmopt_server
python3 scripts/bench/matrix_runner.py \
  --config scripts/bench/matrix/phase12_s3.json \
  --out evidence/phase12/perf --run-id s3-a-resident
```

产物：`matrix_report.csv`、`deviation.csv`、`step_summary.csv`、`repro_manifest.json`、`metrics_p*_r*.prom`、`plots/*.png`、`gpu_state.txt`。

## 已知噪声

P2 吞吐低于 P1（轮间偏差 5.5%）、P4/P8 的 TTFT/ITL 轮间偏差 12–15%，说明单机 5 轮仍不足以压到 10% 以内。结论只做同环境相对比较：**吞吐平台 ≈80 tok/s 是稳定的，P2 的单点低于 P1 视为噪声，不作为结论**。

## 下一组对照（配置已就绪，尚未执行）

三组消融分别隔离一个变量，全部 5 轮、同机同模型：

| 配置 | 隔离的变量 | 判据 |
|---|---|---|
| `phase12_s3_mixed.json` | 输出长度 [8,16,32,64,128,256] 循环，`continuous` | 混长下 P8/P1 是否越过 1.3× |
| `phase12_s3_static.json` | 同负载、`batch_policy=static` | continuous 相对 static 的吞吐差，即"补位"净收益 |
| `phase12_s3_prefix_on.json` / `phase12_s3_prefix_off.json` | 384-token 共享前缀，前缀缓存开/关 | P1 与 P8 下 TTFT / prefill token 的差 |

```bash
python3 scripts/bench/matrix_runner.py --config scripts/bench/matrix/phase12_s3_mixed.json \
  --out evidence/phase12/perf-mixed --run-id s3-mixed
python3 scripts/bench/matrix_runner.py --config scripts/bench/matrix/phase12_s3_static.json \
  --out evidence/phase12/perf-static --run-id s3-static
python3 scripts/bench/matrix_runner.py --config scripts/bench/matrix/phase12_s3_prefix_on.json \
  --out evidence/phase12/perf-prefix-on --run-id s3-prefix-on
python3 scripts/bench/matrix_runner.py --config scripts/bench/matrix/phase12_s3_prefix_off.json \
  --out evidence/phase12/perf-prefix-off --run-id s3-prefix-off
```

在四组产物落盘之前，对外只能引用"等长短解码 + 全驻留"这一条边界内的结论。
