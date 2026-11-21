# KV 压力 / 抢占 / chunk 权衡（P=16）

配置：`Qwen3-8B-Q4_K_M`，P=16、ctx=512、`max_tokens=32`、`max_num_batched_tokens=256`、`chunk_size=64`、`q8_0 KV`、`ngl=24`、`donor_slots=0`、关前缀缓存、40 请求。实际每请求 prompt ≈467 token。

| 指标 | 实测 |
|---|---|
| 成功 / 总请求 | **40 / 40** |
| `kv_used_peak` | **461 / 512（90%）** |
| 准入抢占 | 10 |
| chunk 延后 / chunk 步数 | 180 / 296 |
| 连续抢占上限错误 | 0 |
| TTFT p50 / p95 | 13533.706 / 23757.599 ms |
| ITL p99 / p50 | 1833.804 / 793.7 ms |
| 总 token 吞吐 | 190.9 tok/s（prompt 主导，40×467 prefill token） |
| 进程显存 | 3570.96 MiB（`buffers`） |

结论：这组参数真正打到块表水位，抢占与 chunk 延后均触发；代价是 TTFT 从毫秒级涨到 13.5 s、单路 decode 约 1.26 tok/s。**不能把 190.9 tok/s 当生成吞吐**，它是高并发 prefill 主导的总 token 速率。

```bash
python3 scripts/bench/matrix_runner.py --config scripts/bench/matrix/phase8b_kv_pressure.json \
  --out bench/results/phase8b_kvpressure
```
