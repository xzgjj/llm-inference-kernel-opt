# 部署与运维（阶段 6）

单机单进程服务，最小部署面是「一个二进制 + 一个模型文件 + 一份 JSON 配置」。健康检查用 `/readyz`（就绪）与 `/healthz`（存活），排空由 SIGTERM 触发。

## 容器

```bash
docker build -f scripts/release/Dockerfile -t llmopt:dev .
docker run --rm -p 8080:8080 -e LLMOPT_ALLOW_STUB=1 \
  -v "$PWD/models:/app/models:ro" llmopt:dev \
  --model /app/models/qwen2.5-0.5b-q4.gguf --host 0.0.0.0 --port 8080 \
  --backend stub --mode off
```

镜像默认 `LLMOPT_ENABLE_LLAMA=OFF`（只带 stub），真后端需在构建时打开开关并准备 llama.cpp；模型不进镜像，运行时挂载。`docker_smoke.sh` 用伪造 GGUF 跑通「起服务 → readyz 200 → chat 200 → SIGTERM 退出 0」。

真后端镜像（D2-07）：

```bash
docker build -f scripts/release/Dockerfile \
  --build-arg LLMOPT_ENABLE_LLAMA=ON -t llmopt:0.13.0-llama .
docker run --gpus all --rm -p 8080:8080 \
  -v "$PWD/models:/app/models:ro" llmopt:0.13.0-llama \
  --model /app/models/Qwen3-8B-Q3_K_M.gguf --host 0.0.0.0 --port 8080 \
  --ctx-tokens 1024 --n-gpu-layers 37 --mode auto --log-level warn
```

- builder 内按 `LLAMA_REVISION=b12557` 拉 llama.cpp 只编 `llama`/`ggml` 共享库，runtime 只拷 `/opt/llama.cpp/build/bin/*.so*` 并设 `LD_LIBRARY_PATH=/app/lib`；基础镜像与 builder 同为 Ubuntu 24.04，避免 glibc 不匹配。
- runtime 仍是非 root `uid 10001`，模型只读挂载，镜像 label 带 `llmopt.backend` 与 `llmopt.llama.revision`。
- CUDA 变体不在默认路径：需要 `nvidia/cuda:12.4-runtime-ubuntu24.04` 作 runtime 基础镜像，builder 加 `-DGGML_CUDA=ON`；本仓库未验证该组合，写在这里供后续照做。
- **取证状态：BLOCKED**。本机 Windows 无 Docker、WSL 内无 docker CLI，镜像构建与六项冒烟（healthz / readyz / 非流式 / SSE / 断连取消 / `docker stop` rc=0）都没有跑过。补齐条件：在装有 Docker 的机器上执行上面两条命令 + `scripts/release/docker_smoke.sh`，产物放 `evidence/phase13/packaging/`。

## systemd

`scripts/release/llmopt.service` 是模板：`ExecStart` 读 `/etc/llmopt/config.json`，`KillSignal=SIGTERM`、`TimeoutStopSec=45`（比默认 drain 30 s 多留余量）。文本日志落 `logs/server/llmopt.log`（20 MB × 5 轮转）；事件日志落 `logs/server/events.jsonl`（50 MB × 7 轮转，成功请求 1/8 采样、失败全量）。两路合计上限 450 MB，由 `scripts/log_hygiene.py --limit-mb 450` 守护。也可用 journald 收 stdout。

自愈（D2-11）：`Restart=on-failure` + `RestartSec=5` + `StartLimitIntervalSec=60` + `StartLimitBurst=3`。进程被 kill -9 或异常退出后 5 s 内拉起；60 s 内连挂 3 次进入 `failed`，避免配置错误无限重启。校验：

```bash
systemd-analyze verify scripts/release/llmopt.service
```

外部探针 `scripts/release/probe.sh`：连续抓 3 次 `/readyz`，任一次非 200 就打 `ALERT` 行并退出 1；全绿退出 0；缺 curl 退出 2。适合 cron 每分钟跑：

```bash
LLMOPT_PROBE_URL=http://127.0.0.1:8080/readyz \
LLMOPT_PROBE_LOG=/var/log/llmopt/probe.log \
scripts/release/probe.sh
```

故障注入回归：`integration_fault_injection`（runner 挂死、慢客户端、dump 写失败）与 `integration_server_tls_config_error`（TLS 配置错）共同覆盖四类故障面。

## 慢请求排障（D2-10）

- 打开：`--slow-request-ms 1000`（默认 0=关；`LLMOPT_SLOW_REQUEST_MS` 等价）。阈值从**入队**算到**收尾**。
- 落盘：`--slow-dump-dir logs/slow`（默认即 `logs/slow`）。一条请求一个 `<req_id>.jsonl`，追加写；目录保留最近 20 份，单文件上限 8 MB。
- 内容：`queue_wait_ms` / `prefill_ms` / `first_token_ms` / `total_ms` 四段，加 `prompt_tokens`、`output_tokens`、`preemptions`、`finish`、`prompt_sha8`、`prompt_redacted=true`。**不写 prompt 原文与生成文本**，只写 token 数与 token 序列的 sha256 前 8 位。
- 回放：`python tools/dev.py trace <req_id>`（默认读 `logs/slow`，`--dir` 可改，`--json` 给脚本用，找不到退出 2）。
- 服务端同时打一条 `slow request dump req_id=… total_ms=…` 的 info 日志，便于按 id 从 journald 反查。

## 夜间作业

`.github/workflows/nightly.yml`：

- TSan 跑 fast 套件 10 轮，0 race 才通过；
- ASan/UBSan 跑一轮 fast；
- `scripts/release/soak.py` 30 min 持续请求，断言无非 200、RSS 相对基线增长 <5%，报告写 `build/evidence/soak_report.json`。

## 本机限制

Windows 本机没有 Docker 与 SIGTERM 语义：`docker_smoke.sh` 与 `service_state_test` 只在 Linux/CI 真跑，本机以 `scripts/release/soak.py --seconds 30` 做短时冒烟（已验证 1566 请求 / 0 错误 / RSS +0.95%）。
