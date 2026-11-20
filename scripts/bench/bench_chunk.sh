#!/usr/bin/env bash
# bench_chunk.sh — 3-G2：{32,64,128,256} 四档 chunk 曲线，每档 30 请求、两轮取中。
# 用法：wsl -e bash -lc 'cd /mnt/d/c++_myproject/llm-inference-kernel-opt && bash scripts/bench/bench_chunk.sh'
set -euo pipefail

cd "$(dirname "$0")/../.."

SERVER=${SERVER:-./build-wsl/llmopt_server}
MODEL=${MODEL:-/root/llm-src/models/qwen2.5-0.5b-q4.gguf}
PORT=${PORT:-18100}
REQUESTS=${REQUESTS:-30}
ROUNDS=${ROUNDS:-2}
MAX_TOKENS=${MAX_TOKENS:-32}
CONCURRENCY=${CONCURRENCY:-4}
MAX_CONCURRENCY=${MAX_CONCURRENCY:-4}
BATCH_TOKENS=${BATCH_TOKENS:-256}
PROMPT_REPEAT=${PROMPT_REPEAT:-20}
NGL=${NGL:-99}
THREADS=${THREADS:-0}
OUT=${OUT:-evidence/chunk_tradeoff.csv}
RAW_DIR=${RAW_DIR:-/tmp/llmopt-chunk-raw}
LOG=${LOG:-/tmp/llmopt-chunk-bench.log}

mkdir -p "$RAW_DIR" "$(dirname "$OUT")"
rm -f "$RAW_DIR"/*.csv

for chunk in 32 64 128 256; do
    for round in $(seq 1 "$ROUNDS"); do
        echo "=== chunk=$chunk round=$round ==="
        rm -f "$LOG"
        "$SERVER" --model "$MODEL" --host 127.0.0.1 --port "$PORT" --backend llama \
            --mode off --ctx-tokens 1024 --max-concurrency "$MAX_CONCURRENCY" \
            --max-num-batched-tokens "$BATCH_TOKENS" --chunk-size "$chunk" \
            --block-size 16 --n-gpu-layers "$NGL" --threads "$THREADS" \
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

        python3 scripts/bench/bench_cb.py --port "$PORT" --concurrency "$CONCURRENCY" \
            --requests "$REQUESTS" --max-tokens "$MAX_TOKENS" \
            --prompt-repeat "$PROMPT_REPEAT" \
            --output "$RAW_DIR/chunk_${chunk}_r${round}.csv"

        kill "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
        trap - EXIT
    done
done

python3 scripts/bench/aggregate_chunk.py --raw-dir "$RAW_DIR" --output "$OUT"
echo "写出 $OUT"
