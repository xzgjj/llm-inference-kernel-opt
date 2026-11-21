# llm-inference-kernel-opt

**A single-machine, single-process C++20 LLM inference service testbed**

[简体中文](README.md) ｜ [English](README.en.md)

## Overview

In the author's own words (translated from the original Chinese):

> This project is centered on llama-related work, mainly around a C++ LLM inference service. The main directions in C++ LLM work are listed below; based on my capability and hardware, I chose to focus on serving and scheduling.
>
> - Operator / backend: pluggable ggml backends, custom Backend and operator acceleration
> - **Serving and scheduling: a C++ LLM inference service with Continuous Batching and request scheduling**
> - Algorithm acceleration: speculative decoding and end-to-end speedup evaluation
> - Low-bit quantization: accuracy vs throughput trade-offs under INT4 / FP8

No hand-written GEMM here: model loading, graph construction and kernel execution reuse ggml / llama.cpp, while everything self-built lives in the control plane — admission, batching, the KV block table, chunked prefill, preemption and recovery, capacity gating, per-tenant quotas and metrics. The question this repo answers is concrete: on a given machine, how much do continuous batching and chunked prefill actually buy, where does control-plane overhead show up, and where are the boundaries.

The `kernel` in the repository name means the **service kernel** (scheduling and KV management), not a CUDA kernel. The project does not implement custom operators, distributed inference or training. Scope is one machine, one process, 0.5B–27B quantized models; the measured tier is Qwen3-8B Q4_K_M, and the 27B tier is used only for capacity/preemption checks, never for throughput claims.

Stages 0–11 are landed (stage 10 = real-backend nightly reliability, stage 11 = static key file + per-tenant quotas + KV domain isolation). Stage 12 is in progress (`docs/SPEC-DEBUG1.md`: defect-surface fixes and compute-amplification containment). `ctest -L fast` is green and the registered-case count is generated in [`docs/testing.md`](docs/testing.md). On Linux/WSL the real llama.cpp backend serves both streaming and non-streaming with Qwen3-8B Q4_K_M; Qwen2.5-0.5B is kept as the CI smoke tier.

## Key Features

1. **Continuous batching**: token-level batching with running sequences separated from the waiting queue. Every step is bounded by the `max-num-batched-tokens` budget; requests that do not fit go to the waiting queue, and a full queue returns 429.
2. **Block-table KV management**: fixed-size blocks (16 tokens by default) with reference counting, full-block hashing and copy-on-write. The block table is the sole owner of KV allocation decisions; backend cells are a downstream mapping only and never write back.
3. **Prefix reuse**: full blocks with identical prefixes can be shared and can be disabled with `--disable-prefix-cache`; the cache degrades to no sharing when the backend cannot do zero-copy sharing.
4. **Chunked prefill**: long prompts are split by `--chunk-size` (32 / 64 / 128 / 256) and mixed into decode batches. Only the last chunk samples; earlier chunks only advance KV.
5. **Preemption and recovery**: when blocks are insufficient, a victim is chosen with a cost model and recovered by recompute. Waiting-request aging exemptions and a per-request consecutive preemption cap are supported, and preemption events can be traced to CSV.
6. **Minimal OpenAI-compatible surface**: `/v1/completions`, `/completion`, `/v1/chat/completions` and `/v1/models`; streaming uses SSE with one token per frame and terminates with `data: [DONE]`.
7. **Capacity gating**: RAM / free GPU memory is probed at startup, and `required ≤ free × 0.80` yields `ELIGIBLE / DEGRADED / BLOCKED` with `off / auto / force_on` modes. Default is `off`.
8. **Operability**: four-level config precedence (CLI > env > JSON > defaults), `/healthz` and `/readyz`, SIGTERM drain, token-bucket rate limiting, static key-file auth with per-tenant concurrency / queue / rate / KV quotas (stage 11), optional in-process TLS plus a loopback control listener (stage 9), and Prometheus text metrics with `llmopt_tenant_*` labels.
9. **Experiments and evidence**: `tools/bench` handles request generation, SSE collection, aggregation and plotting; results are archived per `evidence/phaseN/runs/<run_id>/` so CSV, JSONL and logs stay reproducible.

## Tech Stack

| Area | Choice |
| --- | --- |
| Language | C++20: `std::span`, `std::optional`, `std::atomic`, RAII, `enum class`; no modules, coroutines or ranges on hot paths |
| Build | CMake ≥ 3.20 with FetchContent (dependencies land in `third_party/_deps`, never committed); Ninja optional |
| Inference backend | ggml + llama.cpp at a pinned revision (`b12557`) behind `LLMOPT_ENABLE_LLAMA`; the revision is checked by `cmake/deps.cmake` (`LLMOPT_LLAMA_EXPECTED_REVISION`, strict mode optional); a stub backend is the default for CI and routing self-checks |
| HTTP / SSE | cpp-httplib 0.18.5 (single header; OpenSSL, zlib and brotli disabled; 0.18.5 adds `is_connection_closed`) |
| JSON | nlohmann/json 3.11.3 |
| Logging | spdlog 1.17.0 (header-only, bundled fmt) |
| Concurrency model | HTTP thread pool → bounded `SchedulerMailbox` (`std::deque` + `mutex` + `condition_variable`, self-written; no external lock-free queue) → single-threaded `SchedulerCore` mutating state without locks → execution thread as the only KV writer |
| Testing | doctest 2.4.11 + CTest; ASan / UBSan / TSan; Python integration and e2e scripts |
| Metrics | Self-written Prometheus text exposition (counter / gauge); quantiles recomputed offline (no external benchmark framework) |
| Scripts | Python 3.10+ (standard library only) for CI gates, benchmarks and capability probing |
| Platform baseline | Windows 11: g++ 16.2 / clang 22.1.8 / CMake 3.30.3 (stub backend); WSL Ubuntu 26.04: real llama.cpp backend |

Dependency budget: at most 5 direct third-party libraries (doctest, cpp-httplib, nlohmann, spdlog, llama.cpp). No Python at runtime. No database.

## Quick Start

Prerequisites: a C++20 compiler (GCC 13+ or Clang 16+ verified), CMake ≥ 3.20, Python 3.10+ (tooling and CI only). GPU is optional; the CPU path must pass all unit tests and the single-request loop.

### 1. Build and test (stub backend, no model weights)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build -L fast --output-on-failure
```

The binary is `build/llmopt_server` (or `build/llmopt_server.exe` on Windows).

### 2. Stub self-check (HTTP → scheduler → SSE)

```powershell
python tests\fixtures\models\make_fake_gguf.py tests\fixtures\models\fake.gguf
$env:LLMOPT_ALLOW_STUB = "1"
.\build\llmopt_server.exe --model tests\fixtures\models\fake.gguf --backend stub `
  --port 18080 --ctx-tokens 512 --mode off --log-level warn
```

```bash
curl -s http://127.0.0.1:18080/healthz
curl -N -X POST http://127.0.0.1:18080/completion \
  -H 'Content-Type: application/json' \
  -d '{"prompt":"hello","stream":true,"max_tokens":16}'
```

### 3. Real backend (Linux / WSL, with llama.cpp prebuilt)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DLLMOPT_ENABLE_LLAMA=ON -DLLMOPT_LLAMA_ROOT=/root/llm-src/llama.cpp
cmake --build build --parallel
./build/llmopt_server --model /root/llm-src/models/Qwen3-8B-Q4_K_M.gguf \
  --port 8080 --ctx-tokens 2048 --max-num-batched-tokens 512 \
  --chunk-size 64 --max-concurrency 8 --mode auto --threads 8 --n-gpu-layers 24
```

With `LLMOPT_ENABLE_LLAMA=OFF` only the stub backend is built. Windows lacks a CUDA toolchain and SIGTERM semantics here, so real-backend and deployment cases are validated on Linux/CI.

### 4. Single entry point for development

| Command | Purpose |
| --- | --- |
| `python tools/dev.py env` | Print toolchain, memory, GPU and model-path snapshot |
| `python tools/dev.py build` | Unified build (Debug / Release) |
| `python tools/dev.py test` | Run fast / slow cases from the registry |
| `python tools/dev.py run` | Start the service with a config |
| `python tools/dev.py diagnose` | Collect build, test and runtime diagnostics |
| `python scripts/ci/run_ci.py --mode fast` | Run the same gates as CI locally (docs + registry + code) |
| `python scripts/ci/run_ci.py --mode pr` | Add diff budget and code-quality checks |
| `python scripts/ci/check_coverage.py --build-dir build --base origin/main` | Coverage gate (`src/` lines ≥ 70%, changed lines ≥ 80%) |
| `python scripts/ci/gen_test_counts.py --check` | Verify documented test counts against `ctest -N` |

`dev.py` exit codes: 0 success, 1 failure, 2 usage error, 3 missing environment, 4 skipped.

## Environment Variables

Every service flag has an environment-variable counterpart. Precedence is **CLI > `LLMOPT_*` > `--config` file > built-in default**. Common ones are listed below; the full table lives in [`CLI.md`](CLI.md).

| Variable | Default | Meaning |
| --- | --- | --- |
| `LLMOPT_MODEL` | required | GGUF path; metadata is read for budgeting, weights are not loaded here |
| `LLMOPT_CONFIG` | empty | Flat JSON config file (≤ 16 MiB); unknown keys, wrong types or out-of-range values exit with code 2 |
| `LLMOPT_HOST` / `LLMOPT_PORT` | `127.0.0.1` / `8080` | Bind address and port; loopback only by default |
| `LLMOPT_CTX_TOKENS` | `2048` | Context limit, also used for KV estimation and overflow rejection |
| `LLMOPT_MAX_CONCURRENCY` | `8` | Maximum concurrently running sequences; extra requests wait in the queue |
| `LLMOPT_MAX_NUM_BATCHED_TOKENS` | `2048` | Per-step token budget; may be below `ctx-tokens` thanks to chunked prefill |
| `LLMOPT_QUEUE_CAPACITY` | `32` | Waiting-queue limit; full returns 429 `QUEUE_FULL` |
| `LLMOPT_BLOCK_SIZE` | `16` | Tokens per logical KV block, also the prefix-match granularity |
| `LLMOPT_CHUNK_SIZE` | `128` | Chunked-prefill step width; only 32 / 64 / 128 / 256 |
| `LLMOPT_MODE` / `LLMOPT_SAFETY_RATIO` | `off` / `0.80` | Capacity-gating mode and soft budget ratio (0.80–0.95) |
| `LLMOPT_BACKEND` | `llama` | `llama` or `stub`; stub requires `LLMOPT_ALLOW_STUB=1` |
| `LLMOPT_API_KEY` | empty | Single-token Bearer auth; empty disables it, startup summary prints only set/unset |
| `LLMOPT_API_KEYS_FILE` | empty | Stage-11 static key file; JSON stores only SHA-256 digests and tenant quotas, POSIX mode must be 0600 |
| `LLMOPT_RATE_LIMIT_RPS` | `0` | Global token-bucket rate; 0 disables. Over-limit returns 429 `RATE_LIMITED` + `Retry-After: 1` |
| `LLMOPT_DRAIN_DEADLINE_SECONDS` | `30` | Upper bound for draining in-flight requests after SIGTERM |
| `LLMOPT_ALLOW_STUB` | unset | Set to `1` to permit the stub backend for tests and CI |
| `LLMOPT_LLAMA_ROOT` | `/root/llm-src/llama.cpp` | Root CMake uses to find llama.cpp headers and libraries |

There is no `DATABASE_URL`: the project intentionally has no database or persistence layer.

## Project Structure

```text
llm-inference-kernel-opt/
├─ src/                  # Production code: HTTP, protocol, scheduler, preemption, KV, gating, auth, metrics, backends
│  ├─ http_server.cpp    # Routing and SSE write-back; handlers only build request objects and enqueue
│  ├─ protocol.cpp       # Request JSON parsing and SSE frame serialization
│  ├─ scheduler.cpp      # Admission, batching, token budget, chunk planning, tenant snapshots
│  ├─ preempt.cpp        # Preemption cost model, victim selection, recompute recovery
│  ├─ kv.cpp             # Block table, reference counting, prefix hashing, CoW
│  ├─ runner_stub.cpp    # Fake backend so the control plane runs without weights
│  ├─ runner_llama.cpp   # llama.cpp adapter: batch → ubatch → compute → sampling
│  ├─ gate.cpp           # Capacity probing and three-state verdict
│  ├─ metrics.cpp        # Prometheus text metrics
│  └─ main.cpp           # Argument parsing, startup budget, signals and drain
├─ include/llmopt/       # Public interfaces and data structures (scheduler / sequence / kv / sink / runner / tenant ...)
├─ tests/                # unit / contract / integration / e2e / perf plus Python service fixtures
├─ tools/                # dev.py entry point; bench collection, aggregation, plotting, matrix orchestration
├─ scripts/              # CI gates, bench repro scripts, release (Dockerfile / systemd / soak)
├─ docs/                 # Planning, SPEC-0…9 + SPEC-DEBUG1, CLI contract, test registry, evidence
├─ cmake/                # FetchContent dependency versions and llama.cpp integration
├─ ci/                   # Self-tests for the gate scripts (excluded from product coverage)
└─ .github/workflows/    # ci.yml (L0–L4 gates, scheduled real-backend job) / nightly.yml (TSan, ASan/UBSan, 30-min soak)
```

Layout rules: the top level holds only source, tests, scripts, docs and root config files; depth ≤ 3; no `application/domain/infrastructure` style abstraction layers. CMake targets are `llmopt_core` (common, protocol, sha256, config, kv, preempt, scheduler, sink, metrics, resource, text, gate, gguf, runner_stub, log) plus `llmopt_server` (http_server, main, and runner_llama when the real backend is enabled).

## Usage

### Requests and streaming

```bash
# Streaming completion
curl -N -X POST http://127.0.0.1:8080/completion \
  -H 'Content-Type: application/json' \
  -d '{"prompt":"explain KV cache in one sentence","stream":true,"max_tokens":64,"seed":42}'

# Non-streaming, extract text
curl -s -X POST http://127.0.0.1:8080/v1/completions \
  -H 'Content-Type: application/json' \
  -d '{"prompt":"hello","max_tokens":8}' | jq -r '.choices[0].text'
```

Request bodies are capped at 4 MB. Main fields: `prompt` (required), `id`, `stream`, `max_tokens` / `n_predict` (default 128), `top_k`, `top_p`, `min_p`, `temperature`, `repeat_penalty`, `seed`, `stop` (up to 4 entries), `n` (1–4, non-streaming only). The sampling chain is fixed at **penalty → top-k → top-p → min-p → temperature → dist**; requests cannot reorder it.

The chat endpoint accepts plain-text `system / user / assistant` messages only. `tools`, `tool_choice`, `logprobs`, `logit_bias`, `response_format` and image parts return 400 `UNSUPPORTED_FIELD` instead of being silently ignored.

### Errors and probes

Errors always take the shape `{"error":{"code":"QUEUE_FULL","message":"...","retryable":true}}`.

| Case | HTTP | code |
| --- | --- | --- |
| Queue full | 429 | `QUEUE_FULL` |
| Rate limited | 429 | `RATE_LIMITED` |
| KV below watermark | 429 | `KV_BELOW_WATERMARK` |
| Context overflow | 400 | `CONTEXT_OVERFLOW` |
| Auth failure | 401 | `UNAUTHORIZED` |
| Tenant quota exhausted | 429 | `QUOTA_EXCEEDED` |
| KV or resource shortage | 507 | `KV_INSUFFICIENT` / `RESOURCE_INSUFFICIENT` |
| Execution failure | 500 | `EXECUTION_FAILED` |

```bash
curl -s http://127.0.0.1:8080/healthz   # liveness, always 200, body reports starting|ready|draining|stopped
curl -s http://127.0.0.1:8080/readyz    # readiness, 200 only when ready, otherwise 503
curl -s http://127.0.0.1:8080/metrics | grep llmopt_requests_total
```

Process exit codes: 0 normal, 1 startup failure, 2 usage error, 3 insufficient resources, 4 gating refusal.

### Benchmarks and reproduction

```bash
python -m tools.bench.collect --config tools/bench/config/phase0_smoke.json \
  --out tools/bench/results/phase0/smoke
python -m tools.bench.aggregate --input tools/bench/results/phase0/smoke
python scripts/spec5/recompute_spec5.py \
  --csv docs/evidence/phase4-official/matrix_report.csv \
  --traces docs/evidence/phase4-official --out docs/evidence/phase5
```

`/metrics` exposes counters and gauges only; p50 / p95 / p99 are recomputed offline by the benchmark tools, which avoids the semantic conflict between online histograms and a self-built backend.

## Current Status and Limitations

Verified surface: stage 0 baseline collection, stage 1 serial closed loop, stage 2 continuous batching and KV reuse, stage 3 chunked prefill and preemption, stage 4 metrics and experiment matrix, stage 6 service completeness, stage 8 8B real-service evidence, stage 9 TLS / control-plane isolation and completion watchdog, stage 10 real-backend nightly reliability, stage 11 static keys and tenant quotas, stage 12 S0 defect fixes (3 P1 + 9 P2). Stage 5 produced pre-research criteria only, no implementation. The real-service e2e suite passes separately.

The registered test count is generated from `ctest -N` and lives in [`docs/testing.md`](docs/testing.md) §2.1 (single source of truth; `scripts/ci/gen_test_counts.py --check` fails CI on drift).

Official 8B matrix (stage-12 S3 re-baseline: WSL Ubuntu 26.04, RTX 4060 Laptop 8 GB, Qwen3-8B Q3_K_M, `ngl 37` fully resident + `q8_0 KV`, 30 requests × 32 tokens × **5 rounds** per cell, real-token accounting):

| Concurrency P | TTFT p50 (ms) | ITL p99 (ms) | Total token throughput (tok/s) | vs P=1 | Scheduler CPU share |
| --- | --- | --- | --- | --- | --- |
| 1 | 28.5 | 23.3 | 77.1 | 1.00× | 0.0002 |
| 2 | 82.6 | 67.7 | 62.0 | 0.80× | 0.0001 |
| 4 | 207.1 | 108.1 | 78.2 | 1.01× | 0.0001 |
| 8 | 320.6 | 171.3 | 80.7 | 1.05× | 0.0001 |

**Verdict: with full residency the throughput gain of continuous batching is disproved** (P8/P1 = 1.05× vs the 1.3× criterion), at the cost of a 7.4× ITL p99 blow-up. Attribution: single execution thread (no step overlap) plus GPU memory bandwidth ceiling — scheduler step p50 is 4–8 µs and `sched_cpu_ratio` stays at 0.01–0.02%, so the control plane is not the bottleneck; 77–81 tok/s is close to this card's effective bandwidth. Throughput plateaus at P=4; beyond that, only queueing and tail latency grow. The older table (`ngl 24`, 13/37 layers on CPU, 1.61× at P=4) reflected a CPU-offload ceiling and is **void**; it is kept only as history. Evidence, per-tier VRAM breakdown and reproduction: `evidence/phase12/perf/README.md`.

Limitations, stated plainly:

- Single machine, single process. Measured coverage is 0.5B–8B quantized models; the 27B tier is used only for capacity and preemption checks, never for throughput conclusions. No distributed inference, tensor parallelism, PD-disaggregated process topology, training, or custom CUDA kernels.
- The real llama.cpp backend is built and validated only on Linux / WSL. Windows focuses on the stub backend and routing tests. Stage 12 S5 built and smoke-tested the container image (83 MB, non-root, stub backend) on Docker Desktop.
- `llama_batch` has no slot field, cells are chosen internally by `find_slot` with no public getter, so block-table/cell reconciliation relies on internal headers. This is a **source-level coupling point**: every llama.cpp upgrade requires re-checking (`b12557` is the pinned revision).
- The compatibility surface is minimal: static key file → tenant + concurrency / queue / rate / KV quotas (stage 11). Stage 12 S4 adds multi-digest rotation and SIGHUP/poll hot reload (POSIX SIGHUP verified under WSL). Token-day quotas are still missing; auth remains an in-process static file, so public multi-tenant deployments should front it with a gateway.
- Phase 9 adds optional in-process TLS (`-DLLMOPT_ENABLE_TLS=ON` + `--tls-cert/--tls-key`) and an optional loopback control listener (`--control-port`); defaults remain plaintext and shared-port.
- Phase 9 adds a completion watchdog (`--completion-timeout-ms`, default 30 s): a missing step completion returns 500 first, and KV is released only after the late completion arrives; the watchdog warns that a permanently hung backend `decode_step` still blocks `stop()` / join.
- The scheduled real-backend job in `ci.yml` uses fixed Qwen2.5-0.5B Q4_K_M + sha256 on CPU, covering healthz/readyz, non-stream, SSE, disconnect-cancel and `/metrics`. Phase 12 adds the disconnect-cancel probe evidence under `evidence/phase12/`.
- `/metrics` has no histograms, so p99 needs offline recomputation. Slow-reader degradation is measured on the real backend; container smoke covers the stub backend (a real-backend image needs `-DLLMOPT_ENABLE_LLAMA=ON` plus llama.cpp runtime libs and was not built this stage).
- Benchmark noise is high: round-to-round TTFT and throughput drift often crosses 10%, so the numbers are not yet suitable for fine-grained tuning decisions. Phase-12 S3 re-runs the batching comparison with a fully GPU-resident configuration (no CPU-offload ceiling) to prove or disprove the batching gain.

## Roadmap

Work follows the existing SPECs and evidence; no new architectural direction is opened:

1. **Stage 12 (SPEC-DEBUG1)**: S1 non-streaming disconnect cancellation; S2 30-minute soak (RSS growth, `kv_used` monotonicity, p99 drift); S3 batching gain re-baseline without the CPU-offload ceiling; S4 key / quota lifecycle (multi-digest rotation, SIGHUP hot reload); S5 packaging and operations (Dockerfile / systemd, graceful stop, log rotation, crash backtrace, in-container stub smoke; real-backend image pending); S6 document-count automation and version closure.
2. **Quality remediation B / C**: stage A already fixed request identity, streaming stop handling, token budget, drain mirroring, preemption prediction and the error surface. B/C items (long-run stability, boundary semantics, doc/contract alignment) are folded into stage 12 where they touch the defect surface.
3. **Chunk tuning validation**: SPEC-5 criteria classify current data as `candidate_pending_chunk`; a post-tuning matrix on the same hardware is required before it can become a trigger.
4. **Baseline stability**: re-measure the stage 0 baseline to shrink round-to-round drift. The GPU path currently only passes `n-gpu-layers` through and has no memory-pressure scheduling policy.
5. **Pre-research entry bar**: the official 14-row recomputation gives 0 trigger / 12 candidates / 2 undecidable for PD disaggregation and speculative decoding — "current data does not support overturning the decision". Any project must start from a new matrix plus measured link bandwidth.

## Deployment

The minimum deployment surface is one binary, one model file and one JSON config.

### Container

```bash
docker build -f scripts/release/Dockerfile -t llmopt:s5 .
docker run --rm -p 8080:8080 -e LLMOPT_ALLOW_STUB=1 \
  -v "$PWD/models:/app/models:ro" llmopt:s5 \
  --model /app/models/qwen3-8b.gguf --host 0.0.0.0 --port 8080 \
  --control-port 8081 --backend stub --mode off
```

The image defaults to `LLMOPT_ENABLE_LLAMA=OFF` (stub only); the real backend requires enabling the switch at build time and providing llama.cpp. Models are never baked into the image — mount them at runtime. The S5 image is multi-stage, runs as uid 10001, and measures 83 MB; `bash scripts/release/docker_smoke.sh` passes all six checks (healthz / readyz / non-stream / SSE / disconnect-cancel / `docker stop` exit 0) with evidence under `evidence/phase12/packaging/`.

### systemd

`scripts/release/llmopt.service` is a template: `ExecStart` reads `/etc/llmopt/config.json`, `WorkingDirectory=/var/lib/llmopt` anchors crash dumps and relative log paths, `KillSignal=SIGTERM`, `TimeoutStopSec=45` (extra headroom over the default 30 s drain), `Restart=on-failure`; logs rotate at 20 MB × 5 plus the deployment-level `scripts/release/logrotate/llmopt`. `systemd-analyze verify` passes.

### Health checks and drain

- Readiness uses `/readyz`; liveness uses `/healthz`.
- After SIGTERM / SIGINT the state becomes `draining`, `/readyz` and new requests return 503 immediately, in-flight requests finish within `--drain-deadline-seconds`, and forced stops are counted while the process still exits with code 0.

### Nightly jobs

`.github/workflows/nightly.yml` runs on a schedule: 10 rounds of the fast suite under TSan (must report zero races), one round under ASan/UBSan, and a 30-minute soak (no non-200 responses, bounded RSS growth). The fixed real-backend job lives in `ci.yml`'s `schedule` trigger. CI gates are layered: L0 pre-commit (10 s), L1 per push (3 min), L2 PR build and coverage (15 min), L3 sanitizers (30 min), L4 nightly (2 h).

## License

[Apache License 2.0](LICENSE)
