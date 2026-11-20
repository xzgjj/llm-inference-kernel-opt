# 6-G6：单机服务镜像（默认 stub 后端；真后端需 LLMOPT_ENABLE_LLAMA=ON 并提供 llama.cpp）。
# 构建：docker build -f scripts/release/Dockerfile -t llmopt:dev .
FROM ubuntu:24.04 AS build
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
    build-essential cmake ninja-build git ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLLMOPT_ENABLE_LLAMA=OFF \
 && cmake --build build --parallel 2 --target llmopt_server

FROM ubuntu:24.04
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
    libstdc++6 ca-certificates curl && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /src/build/llmopt_server /app/llmopt_server
RUN useradd --system --uid 10001 llmopt && mkdir -p /app/logs /app/models \
 && chown -R llmopt:llmopt /app
USER llmopt
EXPOSE 8080
ENTRYPOINT ["/app/llmopt_server"]
CMD ["--host", "0.0.0.0", "--port", "8080", "--backend", "stub", "--mode", "off"]
