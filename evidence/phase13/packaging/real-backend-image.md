# D2-07 真后端镜像取证（2026-11-22）

环境：Windows 11 + WSL2 `Ubuntu-26.04`，Docker `29.1.3`（`dockerd --iptables=false --bridge=none` 起），无 GPU 直通 → 容器内用 CPU 推理。

## 构建

命令：`bash scripts/release/build_images.sh . real`（离线 vendor 路径：`LLMOPT_LLAMA_VENDOR=/root/llm-src/llama.cpp`）。

产物：`llmopt:0.13.0-llama`，size `132119607` 字节（约 126 MiB），labels `llmopt.backend=ON`、`llmopt.llama.revision=b12557`。

构建过程中暴露并修掉的三个问题（均已进代码）：

1. `-DLLMOPT_LLAMA_ROOT` 被静默吞掉：`cmake/deps.cmake` 只读环境变量 `LLMOPT_LLAMA_ROOT`，Dockerfile 的 `-D` 传参退化成默认 `/root/llm-src/llama.cpp` → 容器内报「未找到 llama.cpp 头/库」。改为 `-D` > 环境变量 > 默认 的三级取值。
2. `.dockerignore` 的 `vendor/` 误伤 `scripts/release/vendor/`：离线头文件与 `.so` 进不了构建上下文。改为 `/vendor/` 只排根目录 + `!scripts/release/vendor/`。
3. glibc 不匹配：宿主编的 `libllama.so` 需要 `sqrtf@GLIBC_2.43`，`ubuntu:24.04`（2.39）链接失败。基础镜像统一升 `ubuntu:26.04`（2.43），builder/runtime 同代。

另：`LLMOPT_BUILD_TESTS=OFF` 时仍无条件拉 doctest，容器内下载失败会中断发布构建；`cmake/deps.cmake` 已改为条件拉取。

## 六项冒烟

命令：`bash scripts/release/smoke_image.sh llmopt:0.13.0-llama /mnt/d/ai_model/models/qwen3-8b Qwen3-8B-Q3_K_M.gguf`

```
PASS healthz=200
PASS readyz=200
PASS metrics 三支直方图
PASS completion 200 + choices
PASS 非 root uid=10001
PASS docker stop 退出码 0
SMOKE_RESULT=PASS
```

## 未覆盖

- CUDA 变体：容器内无 GPU 直通（`--gpus all` 在本机 WSL Docker 不可用），未构建、未跑。做法写在 `docs/deploy.md`。
- 网络 clone 路径：本机到 GitHub 的 443 时通时断（`GnuTLS recv error (-110)`），因此实测走 vendor 路径；clone 分支保留在 Dockerfile，逻辑与 vendor 分支共用后续编译参数。
