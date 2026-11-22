# vendor llama.cpp（不入库）

`scripts/release/vendor/llama.cpp/` 是**离线构建真后端镜像**时临时落盘的上游产物：头文件 + `libllama.so*` + `libggml*.so*`，由 `build_images.sh` 从 `LLMOPT_LLAMA_VENDOR` 拷入，构建完即可删除。

- 为什么存在：容器内 `git clone github.com` 在国内网络常被拦（实测 `Failed to connect to port 443`）；离线路径让镜像构建不依赖外网。
- 版本口径：脚本按 `LLAMA_REVISION=b12557` 传 `-DLLMOPT_LLAMA_REVISION_OVERRIDE`，`cmake/deps.cmake` 仍与 `LLMOPT_LLAMA_EXPECTED_REVISION` 交叉校验，严格模式照旧 FATAL。
- 不入库：`.gitignore` 已忽略 `llama.cpp/`；本目录只保留本说明与 `.gitkeep`，Dockerfile 的 `COPY scripts/release/vendor ./vendor` 才总能成立。
