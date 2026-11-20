#!/usr/bin/env bash
# run_cb_bench.sh — 起真实 llama 服务 → 跑 P=1/2/4 吞吐曲线 → 落盘指标 → 收工。
# 用法：wsl -d <distro> -- bash scripts/bench/run_cb_bench.sh
# 可用环境变量覆盖 SERVER/MODEL/PORT/REQUESTS/MAX_TOKENS/CONCURRENCY/NGL/THREADS。
set -euo pipefail

cd "$(dirname "$0")/../.."

SERVER=${SERVER:-./build-wsl/llmopt_server}
MODEL=${MODEL:-/root/llm-src/models/qwen2.5-0.5b-q4.gguf}
PORT=${PORT:-18080}
REQUESTS=${REQUESTS:-8}
MAX_TOKENS=${MAX_TOKENS:-32}
CONCURRENCY=${CONCURRENCY:-1,2,4}
MAX_CONCURRENCY=${MAX_CONCURRENCY:-4}
NGL=${NGL:-99}
THREADS=${THREADS:-0}
OUT=${OUT:-evidence/cb_curve.csv}
METRICS=${METRICS:-evidence/cb_metrics.txt}
LOG=${LOG:-/tmp/llmopt-cb-bench.log}

rm -f "$LOG"
"$SERVER" --model "$MODEL" --host 127.0.0.1 --port "$PORT" --backend llama \
    --mode off --ctx-tokens 1024 --max-concurrency "$MAX_CONCURRENCY" --max-num-batched-tokens 1024 \
    --block-size 16 --n-gpu-layers "$NGL" --threads "$THREADS" --log-level info >"$LOG" 2>&1 &
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
    tail -40 "$LOG" >&2
    exit 1
fi

grep -E "config:|estimate:|decision=" "$LOG" | head -5 || true
python3 scripts/bench/bench_cb.py --port "$PORT" --concurrency "$CONCURRENCY" \
    --requests "$REQUESTS" --max-tokens "$MAX_TOKENS" --output "$OUT"
curl -s "http://127.0.0.1:$PORT/metrics" >"$METRICS"
grep -E "llmopt_(running|kv_|prefix|admission|requests_total|tokens_generated|forward_steps)" \
    "$METRICS" || true

kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
trap - EXIT
