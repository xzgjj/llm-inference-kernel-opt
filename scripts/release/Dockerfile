# 多阶段发布镜像（D2-07）。
#
# 构建：
#   stub（CI/冒烟默认）：
#     docker build -f scripts/release/Dockerfile -t llmopt:0.13.0 .
#   真后端（CPU）：
#     docker build -f scripts/release/Dockerfile \
#       --build-arg LLMOPT_ENABLE_LLAMA=ON -t llmopt:0.13.0-llama .
#   CUDA 变体：本文件不含 CUDA 工具链（镜像会大 2–3 GB、且需要匹配宿主驱动）。
#     做法是用 nvidia/cuda:12.4-runtime-ubuntu24.04 作 runtime 基础镜像，
#     builder 同 CPU 版但加 -DGGML_CUDA=ON；未在本机验证，故不写进默认路径。
#
# 真后端版本固定为 llama.cpp tag b12557（与 cmake/deps.cmake 的
# LLMOPT_LLAMA_EXPECTED_REVISION 一致）。改版本必须同时改两处。
FROM ubuntu:24.04 AS builder
ENV DEBIAN_FRONTEND=noninteractive
ARG APT_MIRROR=mirrors.aliyun.com
ARG LLMOPT_ENABLE_LLAMA=OFF
ARG LLAMA_REVISION=b12557
ARG LLAMA_GIT_URL=https://github.com/ggml-org/llama.cpp.git
RUN echo 'Acquire::Retries "5"; Acquire::http::Timeout "30";' \
        > /etc/apt/apt.conf.d/80-retries \
    && sed -i "s|http://archive.ubuntu.com/ubuntu|http://${APT_MIRROR}/ubuntu|g; \
            s|http://security.ubuntu.com/ubuntu|http://${APT_MIRROR}/ubuntu|g" \
        /etc/apt/sources.list.d/ubuntu.sources \
    && apt-get update -qq && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY include ./include
COPY src ./src
COPY third_party ./third_party
# 真后端：只编共享库（llama + ggml），不装示例与测试；最终只拷 .so 进 runtime。
RUN set -eux; \
    if [ "${LLMOPT_ENABLE_LLAMA}" = "ON" ]; then \
        git clone --depth 1 --branch "${LLAMA_REVISION}" "${LLAMA_GIT_URL}" /opt/llama.cpp; \
        cmake -S /opt/llama.cpp -B /opt/llama.cpp/build -G Ninja \
            -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
            -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=OFF; \
        cmake --build /opt/llama.cpp/build --parallel --target llama ggml; \
    else \
        mkdir -p /opt/llama.cpp/build/bin; \
    fi
RUN set -eux; \
    extra=""; \
    for pair in httplib-src:HTTPLIB nlohmann_json-src:NLOHMANN_JSON spdlog-src:SPDLOG; do \
        d="${pair%%:*}"; n="${pair##*:}"; \
        if [ -d "/src/third_party/_deps/$d" ]; then \
            extra="$extra -DFETCHCONTENT_SOURCE_DIR_$n=/src/third_party/_deps/$d"; \
        fi; \
    done; \
    llama=""; \
    if [ "${LLMOPT_ENABLE_LLAMA}" = "ON" ]; then \
        llama="-DLLMOPT_ENABLE_LLAMA=ON -DLLMOPT_LLAMA_ROOT=/opt/llama.cpp -DLLMOPT_LLAMA_EXPECTED_REVISION=${LLAMA_REVISION}"; \
    fi; \
    cmake -S . -B /src/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DLLMOPT_BUILD_TESTS=OFF \
        ${llama} \
        $extra \
    && cmake --build /src/build --parallel --target llmopt_server

# runtime 阶段：非 root（uid 10001）；只带运行时必需库。
FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
ARG APT_MIRROR=mirrors.aliyun.com
ARG LLMOPT_ENABLE_LLAMA=OFF
ARG LLAMA_REVISION=b12557
LABEL org.opencontainers.image.title="llmopt" \
      org.opencontainers.image.version="0.13.0" \
      org.opencontainers.image.description="C++ continuous-batching inference service" \
      llmopt.backend="${LLMOPT_ENABLE_LLAMA}" \
      llmopt.llama.revision="${LLAMA_REVISION}"
RUN echo 'Acquire::Retries "5"; Acquire::http::Timeout "30";' \
        > /etc/apt/apt.conf.d/80-retries \
    && sed -i "s|http://archive.ubuntu.com/ubuntu|http://${APT_MIRROR}/ubuntu|g; \
            s|http://security.ubuntu.com/ubuntu|http://${APT_MIRROR}/ubuntu|g" \
        /etc/apt/sources.list.d/ubuntu.sources \
    && apt-get update -qq && apt-get install -y --no-install-recommends \
        libstdc++6 libgcc-s1 libgomp1 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
RUN useradd --system --uid 10001 --home /app --shell /usr/sbin/nologin llmopt \
    && mkdir -p /app/models /app/logs /app/lib \
    && chown -R llmopt:llmopt /app
WORKDIR /app
COPY --from=builder /src/build/llmopt_server /app/llmopt_server
# 共享库只拷实际存在的 .so*（CPU 变体为空，指令仍成立）。
COPY --from=builder /opt/llama.cpp/build/bin/*.so* /app/lib/
ENV LD_LIBRARY_PATH=/app/lib
USER llmopt
# 8080 数据面 / 8081 控制面（控制面进程内绑定 loopback，EXPOSE 仅作部署声明）。
EXPOSE 8080 8081
ENTRYPOINT ["/app/llmopt_server"]
CMD ["--host", "0.0.0.0", "--port", "8080", "--control-port", "8081", "--backend", "stub", "--mode", "off"]
