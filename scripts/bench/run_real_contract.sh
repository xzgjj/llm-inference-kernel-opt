#!/usr/bin/env bash
# run_real_contract.sh — 起 8B 真服务，跑 e2e 契约（/healthz + /readyz 口径），落证据。
# 用法：wsl -d Ubuntu-26.04 -- bash scripts/bench/run_real_contract.sh
# 环境变量：SERVER/MODEL/PORT/NGL/CTX_TOKENS/THREADS/OUT/LOG。
set -euo pipefail

cd "$(dirname "$0")/../.."

SERVER=${SERVER:-/root/llmopt-build/llmopt_server}
MODEL=${MODEL:-/mnt/d/ai_model/models/qwen3-8b/Qwen3-8B-Q4_K_M.gguf}
PORT=${PORT:-18080}
NGL=${NGL:-28}
CTX_TOKENS=${CTX_TOKENS:-1024}
THREADS=${THREADS:-12}
OUT=${OUT:-evidence/e2e_8b_contract.txt}
LOG=${LOG:-evidence/e2e_8b_server.log}

if [ ! -x "$SERVER" ]; then
    echo "服务端不存在或不可执行: $SERVER" >&2
    exit 3
fi

"$SERVER" --model "$MODEL" --host 127.0.0.1 --port "$PORT" --backend llama \
    --mode off --ctx-tokens "$CTX_TOKENS" --max-concurrency 4 \
    --max-num-batched-tokens 1024 --block-size 16 \
    --n-gpu-layers "$NGL" --threads "$THREADS" \
    --donor-slots 0 --kv-cache-type q8_0 --disable-prefix-cache \
    --log-level warn >"$LOG" 2>&1 &
PID=$!
trap 'kill "$PID" 2>/dev/null || true' EXIT

ready=0
for _ in $(seq 1 120); do
    if curl -sf -m 2 "http://127.0.0.1:$PORT/readyz" >/dev/null; then
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

mkdir -p "$(dirname "$OUT")"
set +e
LLMOPT_REAL_BASE="http://127.0.0.1:$PORT" \
    python3 tests/e2e/test_real_service_contract.py 2>&1 | tee "$OUT"
rc=${PIPESTATUS[0]}
set -e

kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
trap - EXIT
grep "shutdown: kv_" "$LOG" || true
exit "$rc"
