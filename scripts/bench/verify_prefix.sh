#!/usr/bin/env bash
# verify_prefix.sh — 起真实服务跑同前缀验收：NGL 控制 CPU/GPU，默认 GPU。
set -euo pipefail

cd "$(dirname "$0")/../.."

SERVER=${SERVER:-./build-wsl/llmopt_server}
MODEL=${MODEL:-/root/llm-src/models/qwen2.5-0.5b-q4.gguf}
PORT=${PORT:-18081}
NGL=${NGL:-99}
EXPECT_SHARE=${EXPECT_SHARE:-1}
LOG=${LOG:-/tmp/llmopt-prefix.log}

rm -f "$LOG"
"$SERVER" --model "$MODEL" --host 127.0.0.1 --port "$PORT" --backend llama \
    --mode off --ctx-tokens 1024 --max-concurrency 4 --max-num-batched-tokens 1024 \
    --block-size 16 --n-gpu-layers "$NGL" --log-level warn >"$LOG" 2>&1 &
PID=$!
trap 'kill "$PID" 2>/dev/null || true' EXIT

ready=0
for _ in $(seq 1 80); do
    if curl -sf -m 2 "http://127.0.0.1:$PORT/healthz" >/dev/null; then
        ready=1
        break
    fi
    sleep 0.5
done
if [ "$ready" -ne 1 ]; then
    echo "服务未就绪，日志尾部：" >&2
    tail -30 "$LOG" >&2
    exit 1
fi

python3 scripts/bench/check_prefix.py --port "$PORT" --tokens 16 \
    --expect-share "$EXPECT_SHARE"
kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
trap - EXIT
