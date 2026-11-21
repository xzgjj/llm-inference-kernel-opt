# 部署与运维（阶段 12 S5 收口）

单机单进程服务，最小部署面是「一个二进制 + 一个模型文件 + 一份 JSON 配置」。健康检查 `/readyz`（就绪）与 `/healthz`（存活）；排空由 SIGTERM 触发，`drain-deadline-seconds` 内收尾后退出 0。

## 容器

```bash
# 默认走阿里云 apt 镜像；海外或内网源可 --build-arg APT_MIRROR=archive.ubuntu.com
docker build -f scripts/release/Dockerfile -t llmopt:s5 .
docker run --rm -p 8080:8080 -e LLMOPT_ALLOW_STUB=1 \
  -v "$PWD/models:/app/models:ro" llmopt:s5 \
  --model /app/models/qwen3-8b.gguf --host 0.0.0.0 --port 8080 \
  --control-port 8081 --backend stub --mode off
```

- 多阶段构建：builder（build-essential/cmake/ninja/git）+ runtime（仅 libstdc++/libgcc/ca-certificates），Release、`LLMOPT_ENABLE_LLAMA=OFF`、不编单测，只产 `llmopt_server`。
- `.dockerignore` 保留 `third_party/_deps/*-src`：有本地缓存时用 `FETCHCONTENT_SOURCE_DIR_*` 离线复用，clean clone 无缓存时回退 FetchContent 联网拉取。
- runtime 非 root（uid 10001），`WORKDIR /app`，`EXPOSE 8080 8081`；模型目录约定只读挂载。
- 实测：镜像 **83 MB**（`evidence/phase12/packaging/image_size.txt`），`docker_smoke.sh` 六项全 PASS（healthz/readyz/非流式/SSE/断连取消/docker stop 退出码 0）。

```bash
# 六项冒烟 + drain 退出码，结果写 evidence/phase12/packaging/docker_smoke.json
bash scripts/release/docker_smoke.sh
```

## systemd

`scripts/release/llmopt.service` 是模板，安装后：

```bash
install -d -o llmopt -g llmopt /var/lib/llmopt /var/log/llmopt
cp scripts/release/llmopt.service /etc/systemd/system/
systemctl daemon-reload && systemctl enable --now llmopt
```

要点：`ExecStart` 读 `/etc/llmopt/config.json`；`WorkingDirectory=/var/lib/llmopt`（崩溃 dump 与相对日志路径锚点）；`KillSignal=SIGTERM`、`TimeoutStopSec=45`（比默认 drain 30 s 多留余量）；`Restart=on-failure`。`systemd-analyze verify` 在容器内实测通过。

## 日志轮转与崩溃回溯

- 进程内 spdlog 20 MB × 5 轮转；部署层兜底 `scripts/release/logrotate/llmopt`（`/var/log/llmopt/*.log`，daily × 7、compress、`copytruncate`、`su llmopt llmopt`）。
- 实测：容器内 logrotate `-d`/`-f` 通过，21 MB 假日志切出 `.1` 且原文件截断到 0（`evidence/phase12/packaging/logrotate_test.log`）。
- 崩溃：注册 SIGSEGV/SIGABRT/SIGFPE/SIGILL，dump 写 `<log_dir 父>/crash/crash-<YYYYmmdd-HHMMSS>-<pid>.txt`（含 signal/pid/version、最近日志尾、调用栈），保留最近 2 份；handler 内只走 async-signal-safe 裸系统调用。回归用例 `crash_dump_test`。

## 夜间作业

`.github/workflows/nightly.yml`：

- TSan 跑 fast 套件 10 轮，0 race 才通过；
- ASan/UBSan 跑一轮 fast；
- `scripts/release/soak.py` 30 min 持续请求（4 workers / interval 10 s），五项判据：RSS 末/首 10 min 均值差 ≤30 MiB、ITL p99 末/首 ≤1.3×、drain 后 used=cached=0、reconcile=0、无非 200 且进程存活；报告写 `evidence/phase12/soak/soak_report.json`。

## 本机限制

Windows 本机 Docker Desktop 可用（S5 冒烟已实跑）；`test_server_control_isolation` 等依赖 POSIX 信号语义的用例在 Linux/WSL 侧执行，Windows 侧走 `--api-keys-reload-seconds` 轮询分支。
