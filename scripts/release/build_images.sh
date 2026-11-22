#!/usr/bin/env bash
# 发布镜像构建入口（D2-07）。
# 用法：
#   bash scripts/release/build_images.sh [上下文目录] [stub|real]
# stub：默认，纯 C++ 服务（--backend stub），无外部依赖。
# real：真后端。两种来源二选一：
#   1) 网络：容器内 git clone llama.cpp b12557 并编共享库（需要能访问 GitHub）；
#   2) 离线：设 LLMOPT_LLAMA_VENDOR=<已编好的 llama.cpp 根目录>，脚本只把头文件与
#      libllama/libggml 的 .so 收进 scripts/release/vendor/llama.cpp（不入库）。
# 产物标签：stub → llmopt:0.13.0；real → llmopt:0.13.0-llama。
set -euo pipefail
ctx="${1:-.}"
flavor="${2:-stub}"
vendor="${LLMOPT_LLAMA_VENDOR:-}"
cd "$ctx"

prepare_vendor() {
    local root="$1" out=scripts/release/vendor/llama.cpp
    rm -rf "$out"
    mkdir -p "$out/include" "$out/ggml/include" "$out/build/bin"
    cp -a "$root/include/." "$out/include/"
    cp -a "$root/ggml/include/." "$out/ggml/include/"
    # 只收运行期需要的三个库及其符号链接，跳过示例/测试的可执行文件。
    for lib in "$root"/build/bin/libllama.so* "$root"/build/bin/libggml*.so*; do
        [ -e "$lib" ] && cp -a "$lib" "$out/build/bin/"
    done
    echo "vendored llama.cpp headers+libs from $root -> $out"
}

if [ "$flavor" = "real" ]; then
    if [ -n "$vendor" ]; then
        prepare_vendor "$vendor"
    fi
    # 真后端要 clone GitHub 上的 llama.cpp；容器自建网络在国内常被拦，
    # 因此默认走宿主网络（--network host）。需要隔离时置 LLMOPT_BUILD_NETWORK=default。
    docker build --network "${LLMOPT_BUILD_NETWORK:-host}" \
        -f scripts/release/Dockerfile \
        --build-arg LLMOPT_ENABLE_LLAMA=ON \
        --build-arg LLAMA_REVISION=b12557 \
        -t llmopt:0.13.0-llama .
else
    docker build -f scripts/release/Dockerfile -t llmopt:0.13.0 .
fi

show_features() {
    local tag="$1"
    docker image inspect --format \
        '{{.RepoTags}} backend={{index .Config.Labels "llmopt.backend"}} rev={{index .Config.Labels "llmopt.llama.revision"}} size={{.Size}}' \
        "$tag"
}

if [ "$flavor" = "real" ]; then
    show_features llmopt:0.13.0-llama
else
    show_features llmopt:0.13.0
fi
