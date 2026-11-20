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

## systemd

`scripts/release/llmopt.service` 是模板：`ExecStart` 读 `/etc/llmopt/config.json`，`KillSignal=SIGTERM`、`TimeoutStopSec=45`（比默认 drain 30 s 多留余量）。日志落 `logs/server/llmopt.log`（20 MB × 5 轮转），也可用 journald 收 stdout。

## 夜间作业

`.github/workflows/nightly.yml`：

- TSan 跑 fast 套件 10 轮，0 race 才通过；
- ASan/UBSan 跑一轮 fast；
- `scripts/release/soak.py` 30 min 持续请求，断言无非 200、RSS 相对基线增长 <5%，报告写 `build/evidence/soak_report.json`。

## 本机限制

Windows 本机没有 Docker 与 SIGTERM 语义：`docker_smoke.sh` 与 `service_state_test` 只在 Linux/CI 真跑，本机以 `scripts/release/soak.py --seconds 30` 做短时冒烟（已验证 1566 请求 / 0 错误 / RSS +0.95%）。
