# Q3_K_M 全量上卡（精度降档对照）

配置：`Qwen3-8B-Q3_K_M`（bartowski 转换，非 Qwen 官方仓库；未计算 sha256），`ngl=37/37`、ctx=512、`q8_0 KV`、P=1、32 请求 × 128 token × 2 轮。ctx 从 1024 降到 512，是为桌面占用约 3.6 GB 时留出显存余量。

| 指标 | 实测 |
|---|---|
| decode p50 | **52.14 tok/s**（ITL p50 19.178 ms） |
| 总 token 吞吐 | 58.424 tok/s |
| TTFT p50 / p95 | 29.308 / 110.480 ms |
| ITL p99 | 23.020 ms |
| 进程显存 | 4193.12 MiB（`buffers`） |
| 全卡峰值 | 7479.0 MiB |
| 偏差 | 4.081% / 2.044% / 3.094% → ok |

结论：达到 50 tok/s 单路目标，可作“精度降档”对照档；量化质量损失（perplexity/任务评测）尚未量化，不能当默认生产档。

donor 口径：该轮配置省略 `donor_slots`，实际服务端按默认 donor=4 运行（`server-p1.log` 的 `n_seq_max=5` = P=1 + 4）；矩阵代码已改为始终显式写 `--donor-slots`，本轮结果不受影响。

```bash
python3 scripts/bench/matrix_runner.py --config scripts/bench/matrix/phase8b_q3_full.json \
  --out bench/results/phase8b_q3
```
