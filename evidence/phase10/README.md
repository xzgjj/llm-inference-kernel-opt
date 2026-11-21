# 阶段 10：真后端五部分本地验证

- 日期：2026-11-21
- 脚本：`tests/e2e/nightly_real_backend.py`
- 服务：`/root/llmopt-build/llmopt_server`（LLAMA ON）

## 1. nightly 同款：Qwen2.5-0.5B Q4_K_M，CPU

```bash
python3 tests/e2e/nightly_real_backend.py \
  --server /root/llmopt-build/llmopt_server \
  --model /root/models/qwen2.5-ms.gguf \
  --port 18082 --timeout 120 --threads 4 --ngl 0 --ctx 512 \
  --evidence /root/real-backend-05b.json
```

结果：PASS。模型 sha256 `74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db`（491,400,032 bytes），与 nightly 固定值一致。

固定下载地址：`https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q4_k_m.gguf`。

## 2. 大模型回归：Qwen3-8B Q4_K_M，ngl=24

```bash
python3 tests/e2e/nightly_real_backend.py \
  --server /root/llmopt-build/llmopt_server \
  --model /mnt/d/ai_model/models/qwen3-8b/Qwen3-8B-Q4_K_M.gguf \
  --port 18081 --timeout 180 --threads 8 --ngl 24 --ctx 512 \
  --evidence /root/real-backend-local.json
```

结果：PASS。

## 3. 断言覆盖

- `healthz` / `readyz`：200 且 `status=ready`
- 非流式：200、`choices` 非空、`usage.total_tokens = prompt_tokens + completion_tokens`
- SSE：200、首帧含 `choices`、最后一帧 `data: [DONE]`
- cancel：读取首帧后断开，`llmopt_requests_total{status="cancelled"}` 从 0 增至 1
- `/metrics`：200，含 `llmopt_requests_total` 与 `llmopt_kv_used_blocks`
- 取消后服务仍可完成请求，`llmopt_kv_used_blocks` 为 0

原始证据：

- `real_backend_qwen2.5-0.5b_cpu.json`（默认 LLAMA 构建）
- `real_backend_qwen2.5-0.5b_cpu_ci_build.json`（`LLMOPT_BUILD_TESTS=OFF` 的 CI 同款构建）
- `real_backend_local_qwen3-8b.json`
