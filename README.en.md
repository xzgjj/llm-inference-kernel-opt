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

What the project does: model loading and math go straight to llama.cpp / ggml; the project only handles "traffic control" — who computes first, how many run together, who waits when VRAM runs out, and how KV cache is split and reused. The `kernel` in the repository name means the **service kernel** (scheduling and KV management), not a GPU kernel. No distributed inference or training; scope is one machine, one process, 0.5B–27B quantized models. The question is concrete: **how much do batching, chunking and KV reuse actually buy?** The project does not train models and does not write CUDA kernels.

Qwen3-8B is the main tier (S3 runs `Q3_K_M` fully resident; earlier L1 evidence used `Q4_K_M`); on Windows the stub backend covers scheduling and API tests. It works both as a runnable reference for learning continuous batching and as a minimal OpenAI-compatible service skeleton on top of llama.cpp. The registered-case count appears in the block below.

Runtime status is visualized and observable, and because the API is OpenAI-compatible, existing clients connect directly: point Open WebUI, LibreChat or Chatbox at the base URL and start chatting.

## Version

- Current version **0.13.0** (`project(llmopt VERSION)` in `CMakeLists.txt` is the only source; `--version`, startup logs and `llmopt_build_info` all derive from it). Changelog: [CHANGELOG.md](CHANGELOG.md).
- Rollback in three steps:

1. Code: `git revert <bad-commit>` or `git checkout <previous-tag>`, then `cmake --build build`;
2. Deployment: swap the image/binary back to the previous version and confirm `/readyz` returns 200 with the old `version` in `llmopt_build_info`;
3. Data plane: no persistent state — KV and queues live in memory, so a restart is enough; slow-request dumps and evidence directories can stay.

## Key Features

- Current machine and actual test setup: built first for small-team internal use.

1. **Continuous batching (pack requests together)** — fixes "one request at a time while the GPU waits". Requests are packed token-wise into one batch under the `max-num-batched-tokens` budget; overflow waits in a queue, and a full queue returns 429. Measured (8B fully resident, uniform short decodes, single execution thread): 77.1 tok/s at P1 vs 80.7 tok/s at P8 — **only 5% faster** — while ITL p99 rises from 23.3 ms to 171.3 ms. So "batching always raises throughput" does not hold on this machine for this workload shape: the limit is a single execution thread plus memory bandwidth, and the control plane itself uses 0.01–0.02% CPU. Uniform lengths remove the "short request finishes, slot refills" effect; the mixed-length / static-batch / prefix-toggle comparisons have since been run in D2-03 (see section 8 and `evidence/phase13/README.md`): mixed-length concurrency gains enormously on TTFT (P8 6970 → 307 ms), but throughput only rises from 57.1 to 62.4 tok/s (+9.3%), below the 1.3× criterion.

2. **Block-table KV management (paging for VRAM)** — fixes "KV grows without bound and does not get freed cleanly". KV is cut into fixed 16-token blocks with reference counts and copy-on-write; the block table alone decides allocation, and the backend merely consumes the addresses it is given. Measured: after a 30-minute soak, drain leaves `used=cached=0` and block-table reconciliation differences are 0.

3. **Prefix reuse (do not recompute the same opening)** — fixes wasted prefill on repeated system prompts or shared context. Full blocks with identical prefixes share logical blocks and reference counts, and `--disable-prefix-cache` turns it off. Note: only logical blocks are shared; physical KV is still copied by llama.cpp `seq_cp` or reserved in a target slot — **no zero-copy claim**. In the unit test the second request hits 8 tokens and computes only the remaining 2.

4. **Chunked prefill (slice long prompts)** — fixes "one huge prompt stalls the whole batch". Long prompts are split by `--chunk-size` (32/64/128/256) and mixed with ongoing decode; only the last chunk samples, earlier chunks just advance KV. The unit test splits 320 tokens into 128/128/64 and matches full prefill token by token.

5. **Preemption and recovery (yield when VRAM is tight)** — fixes "not enough resources means OOM or a failed batch". A cost model picks the cheapest victim, frees its KV, and recomputes later when resources return; aging exemptions and a per-request consecutive-preemption cap avoid starvation, and each preemption can be traced to CSV. The rule is **slower rather than OOM or 5xx**.

6. **Minimal OpenAI-compatible surface (existing clients just connect)** — fixes "I do not want to write a custom protocol adapter". Endpoints: `/v1/completions`, `/completion`, `/v1/chat/completions`, `/v1/models`; streaming uses SSE, one token per frame, ending with `data: [DONE]`. Common fields only, not the full OpenAI field set.

7. **Capacity gating (do the math before loading)** — fixes "the model OOMs on startup". RAM and free VRAM are probed, `required ≤ free × 0.80` yields `ELIGIBLE / DEGRADED / BLOCKED` with `off / auto / force_on` (default `off`); when not all layers fit on the GPU, VRAM and RAM are budgeted separately. An 8B model fits an 8 GB card only via partial offload: fully resident weights take 3672 MiB and KV grows to about 918 MiB.

8. **Operability (deployable and debuggable)** — fixes "we cannot see problems, and stopping the service is brutal". Four-level config precedence (CLI > env > JSON > defaults); `/healthz` (alive) and `/readyz` (ready for work); SIGTERM stops accepting, drains in-flight requests, then exits; token-bucket rate limiting; static key-file auth with per-tenant concurrency / queue / rate / KV quotas (stage 11); Prometheus text metrics (counter / gauge) plus three latency histograms (queue wait / TTFT / ITL, D2-09) and `llmopt_tenant_*` tenant labels. Measured: `docker stop` exits 0; a 30-minute soak served 297,441 requests with 0 errors and RSS growth of 0.05 MiB.

9. **Experiments and evidence (claims are reproducible)** — fixes "performance numbers by word of mouth, impossible to re-run". `tools/bench` generates requests, collects SSE, computes TTFT/ITL/throughput and plots; results are archived per `phaseN/runs/<run_id>/` so CSV, JSONL and logs stay reproducible. Stage 12's soak and batching matrix came from this pipeline; raw files live under `evidence/phase12/`.

## Tech Stack

| Area | Choice |
| --- | --- |
| Language | C++20: `std::span` / `std::optional` / `std::atomic` / RAII / `enum class`; no modules, coroutines or ranges on hot paths |
| Build | CMake ≥ 3.20 + FetchContent (dependencies land in `third_party/_deps`, never committed); Ninja optional |
| Inference backend | ggml + llama.cpp (tag `b12557`, optionally strict-checked via `LLMOPT_LLAMA_EXPECTED_REVISION`) behind `LLMOPT_ENABLE_LLAMA`; a stub backend is the default for CI and routing self-checks |
| HTTP / SSE | cpp-httplib 0.18.5 (single header; OpenSSL, zlib and brotli disabled; 0.18.5 provides `is_connection_closed`) |
| JSON | nlohmann/json 3.11.3 |
| Logging | spdlog 1.17.0 (header-only, bundled fmt) |
| Concurrency model | HTTP thread pool → bounded `SchedulerMailbox` (`deque` + `mutex` + `condition_variable`) → `SchedulerCore` mutating state on one thread with no locks → execution thread as the only KV writer |
| Testing | doctest 2.4.11 + CTest; ASan / UBSan / TSan; Python integration and e2e scripts |
| Metrics | Self-written Prometheus text exposition (counter / gauge); quantiles recomputed offline |
| Scripts | Python 3.10+ (standard library only) for CI gates, benchmarks and capacity probing |
| Platform baseline | Windows 11: g++ 16.2 / clang 22.1.8 / CMake 3.30.3 (stub backend); WSL Ubuntu 26.04: real llama.cpp backend |

Dependency budget: at most 5 direct third-party libraries (doctest, cpp-httplib, nlohmann, spdlog, llama.cpp). No Python at runtime. No database.

## Quick Start

Prerequisites: a C++20 compiler (GCC 13+ or Clang 16+ verified), CMake ≥ 3.20, Python 3.10+ (tooling and CI only). GPU is optional; the CPU path must pass all unit tests and the single-request loop.

### Build and test (stub backend, no model weights)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build -L fast --output-on-failure
```

The binary is `build/llmopt_server` (or `build/llmopt_server.exe` on Windows).

### Stub self-check (HTTP → scheduler → SSE)

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

### Backend

- Linux / WSL, with llama.cpp prebuilt

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DLLMOPT_ENABLE_LLAMA=ON -DLLMOPT_LLAMA_ROOT=/root/llm-src/llama.cpp
cmake --build build --parallel
./build/llmopt_server --model /mnt/d/ai_model/models/qwen3-8b/Qwen3-8B-Q4_K_M.gguf \
  --port 8080 --ctx-tokens 1024 --max-num-batched-tokens 1024 \
  --chunk-size 64 --max-concurrency 4 --mode off --threads 12 \
  --n-gpu-layers 30 --kv-cache-type q8_0 --donor-slots 0 --disable-prefix-cache
```

With `LLMOPT_ENABLE_LLAMA=OFF` only the stub backend is built. This Windows machine has no CUDA toolchain and no SIGTERM semantics, so real-backend and deployment cases are validated on Linux/CI.

### Single entry point for development

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

`dev.py` exit codes: 0 success, 1 failure, 2 usage error, 3 missing environment, 4 skipped.

## Environment Variables

Every service flag has an environment-variable counterpart. Precedence is **CLI > `LLMOPT_*` > `--config` file > built-in default**. Common ones are listed below; the full table lives in [`CLI.md`](CLI.md).

| Variable | Default | Meaning |
| --- | --- | --- |
| `LLMOPT_MODEL` | required | GGUF path; metadata is read for budgeting, weights are not loaded here |
| `LLMOPT_CONFIG` | empty | Flat JSON config file; unknown keys, wrong types or out-of-range values exit with code 2 |
| `LLMOPT_HOST` / `LLMOPT_PORT` | `127.0.0.1` / `8080` | Bind address and port; loopback only by default |
| `LLMOPT_CTX_TOKENS` | `2048` | Context limit, also used for KV estimation and overflow rejection |
| `LLMOPT_MAX_CONCURRENCY` | `8` | Maximum concurrently running sequences; extra requests wait in the queue |
| `LLMOPT_MAX_NUM_BATCHED_TOKENS` | `2048` | Per-step token budget; may be below `ctx-tokens` thanks to chunked prefill |
| `LLMOPT_QUEUE_CAPACITY` | `32` | Waiting-queue limit; full returns 429 `QUEUE_FULL`. Data-plane thread budget requires `max-concurrency + queue-capacity + 3 ≤ 256` |
| `LLMOPT_BATCH_POLICY` | `continuous` | `continuous` refills a slot as soon as a sequence finishes; `static` waits for the whole batch to drain (benchmark baseline only) |
| `LLMOPT_BLOCK_SIZE` | `16` | Tokens per logical KV block, also the prefix-match granularity |
| `LLMOPT_CHUNK_SIZE` | `128` | Chunked-prefill step width; only 32 / 64 / 128 / 256 |
| `LLMOPT_MODE` / `LLMOPT_SAFETY_RATIO` | `off` / `0.80` | Capacity-gating mode and soft budget ratio |
| `LLMOPT_BACKEND` | `llama` | `llama` or `stub`; stub requires `LLMOPT_ALLOW_STUB=1` |
| `LLMOPT_API_KEY` | empty | Single-token Bearer auth; empty disables it, summaries print only set/unset; mutually exclusive with `LLMOPT_API_KEYS_FILE` |
| `LLMOPT_API_KEYS_FILE` | empty | Stage-11 static key file; JSON stores only SHA-256 digests and tenant quotas, POSIX mode must be 0600 |
| `LLMOPT_RATE_LIMIT_RPS` | `0` | Global token-bucket rate; 0 disables. Over-limit returns 429 `RATE_LIMITED` + `Retry-After: 1` |
| `LLMOPT_DRAIN_DEADLINE_SECONDS` | `30` | Upper bound for draining in-flight requests after SIGTERM |
| `LLMOPT_REQUEST_DEADLINE_SECONDS` | `120` | Per-request total deadline counted from enqueue, shared by streaming and non-streaming |
| `LLMOPT_ALLOW_STUB` | unset | Set to `1` to permit the stub backend for tests and CI |
| `LLMOPT_LLAMA_ROOT` | `/root/llm-src/llama.cpp` | Root CMake uses to find llama.cpp headers and libraries |

There is no `DATABASE_URL`: the project intentionally has no database or persistence layer.

## Project Structure

```text
llm-inference-kernel-opt/
├─ src/                  # Production code: HTTP, protocol, scheduler, preemption, KV, gating, metrics, backends
│  ├─ http_server.cpp    # Routing and SSE write-back; handlers only build request objects and enqueue
│  ├─ protocol.cpp       # Request JSON parsing and SSE frame serialization
│  ├─ scheduler.cpp      # Admission, batching, token budget, chunk planning
│  ├─ preempt.cpp        # Preemption cost model, victim selection, recompute recovery
│  ├─ kv.cpp             # Block table, reference counting, prefix hashing, CoW
│  ├─ runner_stub.cpp    # Fake backend so the control plane runs without weights
│  ├─ runner_llama.cpp   # llama.cpp adapter: batch → ubatch → compute → sampling
│  ├─ gate.cpp           # Capacity probing and three-state verdict
│  ├─ metrics.cpp        # Prometheus text metrics
│  └─ main.cpp           # Argument parsing, startup budget, signals and drain
├─ include/llmopt/       # Public interfaces and data structures (scheduler / sequence / kv / sink / runner ...)
├─ tests/                # unit / contract / integration / e2e / perf plus Python service fixtures
├─ tools/                # dev.py entry point; bench collection, aggregation, plotting, matrix orchestration
├─ scripts/              # CI gates, bench repro scripts, release (Dockerfile / systemd / soak)
├─ docs/                 # Planning, SPEC-0…9 + SPEC-DEBUG1, CLI contract, test registry, research and perf evidence
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

### Errors

Errors always take the shape `{"error":{"code":"QUEUE_FULL","message":"...","retryable":true}}`.

| Case | HTTP | code |
| --- | --- | --- |
| Queue full | 429 | `QUEUE_FULL` |
| Transport backlog full | 503 | `SERVICE_UNAVAILABLE` |
| Rate limited | 429 | `RATE_LIMITED` |
| Tenant quota exhausted | 429 | `QUOTA_EXCEEDED` |
| KV below watermark | 429 | `KV_BELOW_WATERMARK` |
| Context overflow | 400 | `CONTEXT_OVERFLOW` |
| Auth failure | 401 | `UNAUTHORIZED` |
| KV or resource shortage | 507 | `KV_INSUFFICIENT` / `RESOURCE_INSUFFICIENT` |
| Execution failure | 500 | `EXECUTION_FAILED` |

```bash
curl -s http://127.0.0.1:8080/healthz   # liveness, always 200, body reports starting|ready|draining|stopped
curl -s http://127.0.0.1:8080/readyz    # readiness, 200 only when ready, otherwise 503
curl -s http://127.0.0.1:8080/metrics | grep llmopt_requests_total
curl -s http://127.0.0.1:8080/metrics | grep 'llmopt_tenant_.*tenant="tenant-a"'
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

`/metrics` exposes counters and gauges, and D2-09 added three online histograms (`llmopt_latency_queue_wait_ms`, `llmopt_latency_ttft_ms`, `llmopt_latency_itl_ms`; the `latency_` prefix deliberately avoids colliding with the existing `llmopt_ttft_ms_sum/_count` series). The offline benchmark tools still recompute p50 / p95 / p99 with the same definitions.

## Current Status and Limitations

- Verified surface: stage 0 baseline collection, stage 1 serial closed loop, stage 2 continuous batching and KV reuse, stage 3 chunked prefill and preemption, stage 4 metrics and experiment matrix, stage 6 service completeness, stage 8 8B real-service evidence, stage 9 TLS / control-plane isolation and completion watchdog, stage 10 real-backend nightly reliability, stage 11 static keys and tenant quotas, stage 12 S0 defect fixes (3 P1 + 9 P2). Stage 5 produced pre-research criteria only, no implementation. The real-service e2e suite passes separately.

<!-- test-counts:start -->
The registered test count is generated from the CTest registry; see [docs/testing.md](docs/testing.md) §2.1 (do not edit by hand).
<!-- test-counts:end -->

- Official 8B matrix (stage-12 S3 re-baseline: WSL Ubuntu 26.04, RTX 4060 Laptop 8 GB, Qwen3-8B Q3_K_M, `ngl 37` fully resident + `q8_0 KV`, 30 requests × 32 tokens × **5 rounds** per cell, real-token accounting):

| Concurrency P | TTFT p50 (ms) | ITL p99 (ms) | Total token throughput (tok/s) | vs P=1 | Scheduler CPU share |
| --- | --- | --- | --- | --- | --- |
| 1 | 28.5 | 23.3 | 77.1 | 1.00× | 0.0002 |
| 2 | 82.6 | 67.7 | 62.0 | 0.80× | 0.0001 |
| 4 | 207.1 | 108.1 | 78.2 | 1.01× | 0.0001 |
| 8 | 320.6 | 171.3 | 80.7 | 1.05× | 0.0001 |

**Verdict: with full residency the throughput gain of continuous batching is disproved** (P8/P1 = 1.05× vs the 1.3× criterion), at the cost of a 7.4× ITL p99 blow-up. Attribution: single execution thread (no step overlap) plus GPU memory bandwidth ceiling — scheduler step p50 is 4–8 µs and `sched_cpu_ratio` stays at 0.01–0.02%, so the control plane is not the bottleneck; 77–81 tok/s is close to this card's effective bandwidth. Throughput plateaus at P=4; beyond that, only queueing and tail latency grow. The older table (`ngl 24`, 13/37 layers on CPU, 1.61× at P=4) reflected a CPU-offload ceiling and is **void**; it is kept only as history. Evidence, per-tier VRAM breakdown and reproduction: `evidence/phase12/perf/README.md`.

**Scope of this verdict**: the workload is uniform 32-token decodes, one prompt tier, P=1/2/4/8, with no static-batch baseline. It shows that raising concurrency from 1 to 8 no longer buys throughput and only costs tail latency; it does **not** show that continuous batching has no value — uniform lengths erase slot reuse, and without a static-batch baseline the 1.05× cannot be attributed specifically to refill. Mixed-length, static vs continuous, and prefix-cache on/off matrices are configured under `scripts/bench/matrix/phase12_s3_*.json`; do not cite a broader conclusion until they have been run.

- **Limitations:** only an early production-grade deployment is possible; an SLA-level performance and redundancy gap remains:
  - no 99.9% or higher availability — there is no second compute resource;
  - no interactive latency promise above concurrency 4 — measured P8 ITL p99 already crosses the line;
  - no context above 1024 / above 8B — the capacity gate returns BLOCKED;
  - no "low latency on long prompts" — with 8B fully resident the VRAM peak is already 7.5 GB / 8.19 GB.

### Capacity and SLO

- SLO targets (hard-coded, valid only for a single instance, single card, 8B Q3_K_M, `ngl 37`, ctx 1024, P≤8):

| Metric | Target | Judgment |
| --- | --- | --- |
| TTFT p50 | ≤ 350 ms | continuous batching, short-prompt tier, P≤8 |
| ITL p99 | ≤ 200 ms | same; exceeding it makes that concurrency tier unusable for interactive traffic |
| Availability | ≥ 99.5% / 30 days | systemd `Restart=on-failure` + external probe (D2-11) |
| Server error rate | ≤ 0.1% | 5xx only; 4xx client errors excluded |

The capacity table is generated by `python scripts/bench/capacity_table.py --evidence evidence/phase13` from D2-03's `matrix_report.csv`; numbers are never hand-written:

| Group | P | TTFT p50 (ms) | ITL p99 (ms) | Total token throughput (tok/s) | VRAM peak (MiB) | Note |
| --- | --- | --- | --- | --- | --- | --- |
| perf-static | 1 | 29.4 | 23.2 | 62.0 | 4384 |  |
| perf-static | 2 | 744.5 | 23.4 | 62.2 | 4461 |  |
| perf-static | 4 | 2160.3 | 70.2 | 56.0 | 4614 |  |
| perf-static | 8 | 6970.0 | 142.1 | 57.1 | 4920 | high† |
| perf-mixed | 1 | 29.8 | 23.3 | 61.0 | 4384 |  |
| perf-mixed | 2 | 70.9 | 51.7 | 51.5 | 4461 | high† |
| perf-mixed | 4 | 168.3 | 112.0 | 61.2 | 4614 | high† |
| perf-mixed | 8 | 307.2 | 208.9 | 62.4 | 4920 | high† |
| perf-prefix-on | 1 | 34.4 | 22.7 | 265.5 | 4384 | insufficient† |
| perf-prefix-on | 4 | 349.1 | 71.8 | 281.0 | 4614 | insufficient† |
| perf-prefix-on | 8 | 654.5 | 134.1 | 332.6 | 4920 | insufficient† |
| perf-prefix-off | 1 | 102.1 | 23.2 | 242.4 | 4078 |  |
| perf-prefix-off | 4 | 411.1 | 73.3 | 290.4 | 4308 | high† |
| perf-prefix-off | 8 | 792.3 | 119.1 | 317.1 | 4614 | high† |

Reading:

- **Interactive capacity tops out at P=4**: at P=8 the ITL p99 is 208.9 ms, past the 200 ms target, while TTFT still passes; at P=4 the ITL p99 of 112 ms and TTFT of 168 ms both stay within target, so that is the highest tier this single card can promise.
- **Static batching's TTFT explodes at P2/P4** (744 ms / 2160 ms) — the unavoidable cost of "wait for the whole batch to drain before refilling"; continuous batching pulls P=8 TTFT down to 307 ms, so the refill gain is real, but throughput only goes from 57.1 to 62.4 tok/s (+9%), **not 1.3×**, so the throughput verdict stands.
- **Prefix caching pays off in TTFT**: with a 384-token shared prefix, P1 drops from 102 ms to 34 ms (3×) and P8 from 792 ms to 654 ms; throughput gains +9.5% at P1 (242.4 → 265.5 tok/s), stays flat at P4 and +5% at P8. The cost is ITL p99 rising from 119 ms to 134 ms at P8 (still under 200 ms). The first prefix-on round crashed on overlapping `seq_cp` positions; it has been fixed with a regression case — see `evidence/phase13/README.md`.
- `deviation_flag=high` means the point is flagged as deviating by a fixed threshold (not a failure); it warns that the point differs too much from its neighbors, so cite the raw `deviation.csv` alongside it. `insufficient` means the group has too few samples under the current threshold and is trend-only.
- 27B throughput, ctx > 1024, P > 8, and multiple instances behind a gateway are outside this stage's conclusions (see the boundaries).

## Roadmap

- Focus on feature evolution and capability guarantees:

1. **Chunk tuning validation**: SPEC-5 criteria classify current data as `candidate_pending_chunk`; a post-tuning matrix on the same hardware is required before it can become a trigger.
2. **Baseline stability**: re-measure the stage 0 baseline to shrink round-to-round drift. The GPU path currently only passes `n-gpu-layers` through and has no scheduling policy under memory pressure.
3. **Pre-research entry bar**: the official 14-row recomputation gives 0 trigger / 12 candidates / 2 undecidable for PD disaggregation and speculative decoding — "current data does not support overturning the decision". Any project must start from a new matrix plus measured link bandwidth.
4. **Deployment surface completion**: S5 has delivered the container image, six smoke checks, logrotate and the systemd unit verification; the real-backend image (llama.cpp runtime libs) and the slow-reader degradation re-test are still to be scheduled.

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

The image defaults to `LLMOPT_ENABLE_LLAMA=OFF` (stub only); the real backend requires enabling the switch at build time and providing llama.cpp. Models are never baked into the image — mount them at runtime. The image is multi-stage, runs as uid 10001, and measures 83 MB; `bash scripts/release/docker_smoke.sh` passes all six checks (healthz / readyz / non-stream / SSE / disconnect-cancel / `docker stop` exit 0) with evidence under `evidence/phase12/packaging/`.

### systemd

`scripts/release/llmopt.service` is a template: `ExecStart` reads `/etc/llmopt/config.json`, `WorkingDirectory=/var/lib/llmopt` anchors crash dumps and relative log paths, `KillSignal=SIGTERM`, `TimeoutStopSec=45` (extra headroom over the default 30 s drain); logs rotate at 20 MB × 5 plus the deployment-level `scripts/release/logrotate/llmopt`. `systemd-analyze verify` passes.

## License

[Apache License 2.0](LICENSE)
