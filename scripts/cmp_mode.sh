#!/usr/bin/env bash
# cmp_mode.sh — 3-G7 对照实验：协作模式实测前缀复用收益 vs 自主模式补丁面。
# 用法：wsl -e bash -lc 'cd /mnt/d/c++_myproject/llm-inference-kernel-opt && bash scripts/cmp_mode.sh'
set -euo pipefail

cd "$(dirname "$0")/.."

SERVER=${SERVER:-./build-wsl/llmopt_server}
MODEL=${MODEL:-/root/llm-src/models/qwen2.5-0.5b-q4.gguf}
PORT=${PORT:-18120}
OUT=${OUT:-evidence/mode_compare.csv}
LOG=${LOG:-/tmp/llmopt-cmp-mode.log}

rm -f "$LOG"
"$SERVER" --model "$MODEL" --host 127.0.0.1 --port "$PORT" --backend llama \
    --mode off --ctx-tokens 1024 --max-concurrency 2 --max-num-batched-tokens 256 \
    --chunk-size 128 --block-size 16 --n-gpu-layers 99 --threads 0 \
    --log-level info >"$LOG" 2>&1 &
PID=$!
trap 'kill "$PID" 2>/dev/null || true' EXIT

ready=0
for _ in $(seq 1 120); do
    if curl -sf -m 2 "http://127.0.0.1:$PORT/healthz" >/dev/null; then
        ready=1
        break
    fi
    sleep 0.5
done
if [ "$ready" -ne 1 ]; then
    echo "服务未就绪，日志尾部：" >&2
    tail -40 "$LOG" >&2
    exit 1
fi

python3 scripts/bench/cmp_mode.py --port "$PORT" --output "$OUT"

kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
trap - EXIT
