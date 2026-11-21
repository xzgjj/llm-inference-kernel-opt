#!/usr/bin/env bash
# run_cb_bench.sh — 起真实 llama 服务 → 跑 P=1/2/4 吞吐曲线 → 落盘指标 → 收工。
# 用法：wsl -d <distro> -- bash scripts/bench/run_cb_bench.sh
# 可用环境变量覆盖 SERVER/MODEL/PORT/REQUESTS/MAX_TOKENS/CONCURRENCY/NGL/THREADS。
set -euo pipefail

cd "$(dirname "$0")/../.."

SERVER=${SERVER:-/root/llmopt-build/llmopt_server}
MODEL=${MODEL:-/mnt/d/ai_model/models/qwen3-8b/Qwen3-8B-Q4_K_M.gguf}
PORT=${PORT:-18080}
REQUESTS=${REQUESTS:-8}
MAX_TOKENS=${MAX_TOKENS:-32}
CONCURRENCY=${CONCURRENCY:-1,2,4}
MAX_CONCURRENCY=${MAX_CONCURRENCY:-4}
BATCH_TOKENS=${BATCH_TOKENS:-1024}
CTX_TOKENS=${CTX_TOKENS:-2048}
NGL=${NGL:-30}
# 部分卸载时 CPU 侧承担 decode 主力：16 逻辑核留 4 个给主线程/GPU 喂数，实测 12 最优。
THREADS=${THREADS:-12}
DONOR_SLOTS=${DONOR_SLOTS:-4}
KV_CACHE_TYPE=${KV_CACHE_TYPE:-f16}
THREADS=${THREADS:-0}
OUT=${OUT:-evidence/cb_curve.csv}
METRICS=${METRICS:-evidence/cb_metrics.txt}
PROMPT_REPEAT=${PROMPT_REPEAT:-1}
PROMPTS_FILE=${PROMPTS_FILE:-}
TIER=${TIER:-short}
WARMUP=${WARMUP:-0}
BASELINE_CSV=${BASELINE_CSV:-}
BASELINE_ROUND=${BASELINE_ROUND:-2}
BASELINE_TIER=${BASELINE_TIER:-short}
ASSERT_GATES=${ASSERT_GATES:-0}
GATES=${GATES:-}
SHUTDOWN_OUT=${SHUTDOWN_OUT:-evidence/kv_shutdown.txt}
LOG=${LOG:-/tmp/llmopt-cb-bench.log}
PREFIX_FLAG=()
if [ "${PREFIX_CACHE:-1}" = "0" ]; then
    PREFIX_FLAG=(--disable-prefix-cache)
fi

rm -f "$LOG"
"$SERVER" --model "$MODEL" --host 127.0.0.1 --port "$PORT" --backend llama \
    --mode off --ctx-tokens "$CTX_TOKENS" --max-concurrency "$MAX_CONCURRENCY" --max-num-batched-tokens "$BATCH_TOKENS" \
    --block-size 16 --n-gpu-layers "$NGL" --threads "$THREADS" \
    --donor-slots "$DONOR_SLOTS" --kv-cache-type "$KV_CACHE_TYPE" --log-level info "${PREFIX_FLAG[@]}" >"$LOG" 2>&1 &
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
extra=()
if [ -n "$BASELINE_CSV" ]; then
    extra+=(--baseline-csv "$BASELINE_CSV" --baseline-round "$BASELINE_ROUND"
            --baseline-tier "$BASELINE_TIER")
fi
if [ "$ASSERT_GATES" = "1" ]; then
    extra+=(--assert-gates --gates "$GATES")
fi
if [ -n "$PROMPTS_FILE" ]; then
    extra+=(--prompts-file "$PROMPTS_FILE" --tier "$TIER" --warmup "$WARMUP")
fi
python3 scripts/bench/bench_cb.py --port "$PORT" --concurrency "$CONCURRENCY" \
    --requests "$REQUESTS" --max-tokens "$MAX_TOKENS" --prompt-repeat "$PROMPT_REPEAT" \
    --output "$OUT" "${extra[@]}"
curl -s "http://127.0.0.1:$PORT/metrics" >"$METRICS"
grep -E "llmopt_(running|kv_|prefix|admission|requests_total|tokens_generated|forward_steps|scheduler|schedule|batch)" \
    "$METRICS" || true

kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
grep "shutdown: kv_" "$LOG" >"$SHUTDOWN_OUT" || true
trap - EXIT
