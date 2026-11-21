# 阶段 10：真后端五部分本地验证

- 日期：2026-11-21
- 脚本：`tests/e2e/nightly_real_backend.py`
- 服务：`/root/llmopt-build/llmopt_server`（LLAMA ON）

## 1. nightly 同款：Qwen2.5-0.5B Q4_K_M，CPU

```bash
python3 tests/e2e/nightly_real_backend.py \
  --server /root/llmopt-build/llmopt_server \
  --model /root/models/qwen2.5-ms.gguf \
  --expect-sha256 74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db \
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
- cancel：读取首帧后断开，`llmopt_requests_total{status="cancelled"}` 恰好 `before + 1`
- `/metrics`：200，含 `llmopt_requests_total` 与 `llmopt_kv_used_blocks`
- 取消后服务仍可完成请求，且 `llmopt_kv_used_blocks == 0`（`kv_released`）
- 模型固定：`--expect-sha256` 在启动服务前校验；不匹配直接非零退出（CI 另有反向用例）

原始证据：

- `real_backend_qwen2.5-0.5b_cpu.json`（默认 LLAMA 构建）
- `real_backend_qwen2.5-0.5b_cpu_ci_build.json`（`LLMOPT_BUILD_TESTS=OFF` 的 CI 同款构建）
- `real_backend_local_qwen3-8b.json`
- `real_backend_qwen2.5-0.5b_cpu_verify.json`（2026-11-21 复验，含哈希强校验与 `kv_released`）

## 4. 2026-11-21 复验：从空环境按 nightly 口径重跑

环境：Ubuntu 22.04、2 vCPU / 3.4 GiB、g++ 11.4、CMake 3.30.3；llama.cpp `b12557` 以 `-DBUILD_SHARED_LIBS=ON -DGGML_NATIVE=OFF` 纯 CPU 构建；服务 `-DLLMOPT_ENABLE_LLAMA=ON -DLLMOPT_BUILD_TESTS=OFF`。

```bash
# 固定 URL 下载 + 哈希校验（491,400,032 bytes）
curl -fSL --retry 3 -o /workspace/models/qwen2.5-0.5b-q4_k_m.gguf \
  https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q4_k_m.gguf
echo "74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db  /workspace/models/qwen2.5-0.5b-q4_k_m.gguf" | sha256sum -c -

python3 tests/e2e/nightly_real_backend.py \
  --server build-real/llmopt_server \
  --model /workspace/models/qwen2.5-0.5b-q4_k_m.gguf \
  --expect-sha256 74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db \
  --port 18090 --timeout 180 --threads 2 --ngl 0 --ctx 512 \
  --evidence /workspace/real-backend-verify.json
```

结果：PASS，9 项布尔检查全真，耗时 2.9 s。`cancelled_before=0`、`cancelled_after=1`、`kv_used_blocks=0`；服务日志收尾 `shutdown: kv_used=0 cached=0 free=32 total=32 shared=0 invariants=ok`。

反向用例：把 `--expect-sha256` 换成 64 个 `0`，脚本在启动服务前退出 1，错误信息同时打印期望值与实际值。

## 5. 三轮 review 与修复

| 轮次 | 检查重点 | 发现 | 处理 |
| --- | --- | --- | --- |
| R1 | 断言真实性 | 旧版只记录 `llmopt_kv_used_blocks`，文档却声称“KV 回零”；cancel 只判 `> before`，无法发现重复计数 | 新增 `kv_released`，最多轮询 5 s 等 KV 归零；cancel 改为精确 `before + 1`，多计直接失败 |
| R2 | fail-closed | 固定 sha256 只存在于 workflow 的 `sha256sum -c`，脚本自身不校验；临时日志名固定，并发跑会互相覆盖 | 脚本新增 `--expect-sha256`，启动前校验；CI 用错误哈希跑反向用例并只接受退出码 1；日志名带端口 |
| R3 | 文档与闸门一致性 | `docs/testing.md`、`ci-spec` 仍写“指标增长”，未提脚本校验；CI 产物路径还是旧日志名 | 三处文档同步为“恰好 +1 + 启动前校验”；上传路径改 `llmopt-nightly-real-18080.log`；`run_ci.py --mode fast` 与 `ctest -L fast` 168/168 通过 |
