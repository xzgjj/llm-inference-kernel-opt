# 阶段 12 S5：多阶段发布镜像（路径保持 scripts/release/Dockerfile 不变，
# docker_smoke.sh、scripts/release/README.md、README.md 均按此路径引用）。
# 构建：docker build -f scripts/release/Dockerfile -t llmopt:s5 .
#
# builder 阶段：Release、stub 后端、不编单测、不装 googletest 运行时，
# 只构建 llmopt_server 目标。FetchContent 依赖在容器内重新拉取（上下文已排除 _deps）。
FROM ubuntu:24.04 AS builder
ENV DEBIAN_FRONTEND=noninteractive
# 国内网络默认走阿里云镜像；海外构建可用 --build-arg APT_MIRROR=archive.ubuntu.com 覆盖。
ARG APT_MIRROR=mirrors.aliyun.com
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
# 依赖获取：若上下文带 third_party/_deps/*-src（.dockerignore 保留源码、排除 build），
# 用 FETCHCONTENT_SOURCE_DIR_* 离线复用，避免容器内再走 GitHub；clean clone 无缓存时
# 自动回退到 FetchContent 联网拉取，两份路径同一份 CMakeLists。
RUN set -eux; \
    extra=""; \
    for pair in httplib-src:HTTPLIB nlohmann_json-src:NLOHMANN_JSON spdlog-src:SPDLOG; do \
        d="${pair%%:*}"; n="${pair##*:}"; \
        if [ -d "/src/third_party/_deps/$d" ]; then \
            extra="$extra -DFETCHCONTENT_SOURCE_DIR_$n=/src/third_party/_deps/$d"; \
        fi; \
    done; \
    cmake -S . -B /src/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DLLMOPT_ENABLE_LLAMA=OFF \
        -DLLMOPT_BUILD_TESTS=OFF \
        $extra \
    && cmake --build /src/build --parallel --target llmopt_server

# runtime 阶段：非 root（uid 10001）；只带运行时必需库。
# 镜像内不含 build 目录、源码、tests、key 文件、evidence；模型目录 /app/models
# 约定只读挂载，日志目录 /app/logs 可写。
FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
ARG APT_MIRROR=mirrors.aliyun.com
RUN echo 'Acquire::Retries "5"; Acquire::http::Timeout "30";' \
        > /etc/apt/apt.conf.d/80-retries \
    && sed -i "s|http://archive.ubuntu.com/ubuntu|http://${APT_MIRROR}/ubuntu|g; \
            s|http://security.ubuntu.com/ubuntu|http://${APT_MIRROR}/ubuntu|g" \
        /etc/apt/sources.list.d/ubuntu.sources \
    && apt-get update -qq && apt-get install -y --no-install-recommends \
        libstdc++6 libgcc-s1 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
RUN useradd --system --uid 10001 --home /app --shell /usr/sbin/nologin llmopt \
    && mkdir -p /app/models /app/logs \
    && chown -R llmopt:llmopt /app
WORKDIR /app
COPY --from=builder /src/build/llmopt_server /app/llmopt_server
USER llmopt
# 8080 数据面 / 8081 控制面（控制面进程内绑定 loopback，EXPOSE 仅作部署声明）。
EXPOSE 8080 8081
ENTRYPOINT ["/app/llmopt_server"]
CMD ["--host", "0.0.0.0", "--port", "8080", "--control-port", "8081", "--backend", "stub", "--mode", "off"]
