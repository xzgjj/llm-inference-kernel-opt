#!/usr/bin/env bash
# phase3_manifest.sh — 3-G5：启动预算三态、真实服务峰值采样、超预算启动拒绝。
# 用法：wsl -e bash -lc 'cd /mnt/d/c++_myproject/llm-inference-kernel-opt && bash scripts/bench/phase3_manifest.sh'
set -euo pipefail

cd "$(dirname "$0")/../.."

SERVER=${SERVER:-./build-wsl/llmopt_server}
MODEL=${MODEL:-/root/llm-src/models/qwen2.5-0.5b-q4.gguf}
PORT=${PORT:-18110}
CTX=${CTX:-1024}
CONCURRENCY=${CONCURRENCY:-4}
REQUESTS=${REQUESTS:-30}
MAX_TOKENS=${MAX_TOKENS:-32}
CHUNK=${CHUNK:-128}
OUT=${OUT:-evidence/phase3_manifest.json}
LIMIT_GIB=${LIMIT_GIB:-4.5}
LOG=${LOG:-/tmp/llmopt-phase3-manifest.log}

rm -f "$LOG"
"$SERVER" --model "$MODEL" --host 127.0.0.1 --port "$PORT" --backend llama \
    --mode off --ctx-tokens "$CTX" --max-concurrency "$CONCURRENCY" \
    --max-num-batched-tokens 256 --chunk-size "$CHUNK" --block-size 16 \
    --n-gpu-layers 0 --threads 0 --log-level info >"$LOG" 2>&1 &
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
    --requests "$REQUESTS" --max-tokens "$MAX_TOKENS" --prompt-repeat 20 \
    --output /tmp/llmopt-phase3-rss.csv >/tmp/llmopt-phase3-rss.json

sleep 0.3
VMHWM_KB=$(awk '/VmHWM/ {print $2}' "/proc/$PID/status")
VM_RSS_KB=$(awk '/VmRSS/ {print $2}' "/proc/$PID/status")
ESTIMATE_LINE=$(grep -m1 'estimate:' "$LOG" || true)
CONFIG_LINE=$(grep -m1 'config:' "$LOG" || true)
DECISION=$(printf '%s' "$ESTIMATE_LINE" | sed -n 's/.*decision=\([A-Z]*\).*/\1/p')

kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
trap - EXIT
SHUTDOWN_LINE=$(grep -m1 'shutdown: kv_' "$LOG" || true)

# 超预算反例：把 ctx 放大到单序列 4M token，KV 估算必然越过 free×safety。
OVER_LOG=/tmp/llmopt-phase3-overbudget.log
set +e
"$SERVER" --model "$MODEL" --backend llama --mode off --ctx-tokens 131072 \
    --max-concurrency 16 --block-size 16 --n-gpu-layers 0 --port 18111 >"$OVER_LOG" 2>&1
OVER_CODE=$?
set -e
OVER_LINE=$(grep -m1 'estimate:' "$OVER_LOG" || true)

# GPU 侧峰值显存：与 CPU RSS 分开记，对应 SPEC-4 的 K-8 VRAM 口径。
GPU_LOG=/tmp/llmopt-phase3-gpu.log
VRAM_FILE=/tmp/llmopt-phase3-vram.txt
rm -f "$GPU_LOG" "$VRAM_FILE"
"$SERVER" --model "$MODEL" --host 127.0.0.1 --port 18112 --backend llama \
    --mode off --ctx-tokens "$CTX" --max-concurrency "$CONCURRENCY" \
    --max-num-batched-tokens 256 --chunk-size "$CHUNK" --block-size 16 \
    --n-gpu-layers 99 --threads 0 --log-level info >"$GPU_LOG" 2>&1 &
GPU_PID=$!
( while kill -0 "$GPU_PID" 2>/dev/null; do
      nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>/dev/null >>"$VRAM_FILE" || true
      sleep 0.2
  done ) &
SAMPLER_PID=$!
trap 'kill "$GPU_PID" "$SAMPLER_PID" 2>/dev/null || true' EXIT
ready=0
for _ in $(seq 1 120); do
    if curl -sf -m 2 "http://127.0.0.1:18112/healthz" >/dev/null; then
        ready=1
        break
    fi
    sleep 0.5
done
if [ "$ready" -ne 1 ]; then
    echo "GPU 服务未就绪，日志尾部：" >&2
    tail -40 "$GPU_LOG" >&2
    exit 1
fi
python3 scripts/bench/bench_cb.py --port 18112 --concurrency "$CONCURRENCY" \
    --requests "$REQUESTS" --max-tokens "$MAX_TOKENS" --prompt-repeat 20 \
    --output /tmp/llmopt-phase3-gpu.csv >/tmp/llmopt-phase3-gpu.json
kill "$GPU_PID" 2>/dev/null || true
wait "$GPU_PID" 2>/dev/null || true
kill "$SAMPLER_PID" 2>/dev/null || true
wait "$SAMPLER_PID" 2>/dev/null || true
trap - EXIT
GPU_ESTIMATE=$(grep -m1 'estimate:' "$GPU_LOG" || true)
PEAK_VRAM_MIB=$(sort -n "$VRAM_FILE" 2>/dev/null | tail -1 || true)

python3 - "$OUT" "$LIMIT_GIB" "$VMHWM_KB" "$VM_RSS_KB" "$ESTIMATE_LINE" \
    "$CONFIG_LINE" "$DECISION" "$SHUTDOWN_LINE" "$OVER_CODE" "$OVER_LINE" \
    "$GPU_ESTIMATE" "$PEAK_VRAM_MIB" <<'PY'
import json
import sys
from datetime import datetime, timezone

(out, limit_gib, hwm_kb, rss_kb, estimate, config, decision, shutdown, over_code,
 over_estimate, gpu_estimate, peak_vram_mib) = sys.argv[1:]
peak_bytes = int(hwm_kb) * 1024 if hwm_kb.isdigit() else None
limit_bytes = int(float(limit_gib) * 1024 ** 3)
vram_mib = int(peak_vram_mib) if peak_vram_mib.isdigit() else None
payload = {
    "phase": "3-G5",
    "generated_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
    "limit_gib": float(limit_gib),
    "config": config.strip(),
    "budget_estimate": estimate.strip(),
    "startup_decision": decision,
    "peak_rss_bytes": peak_bytes,
    "peak_rss_gib": round(peak_bytes / 1024 ** 3, 3) if peak_bytes else None,
    "peak_within_limit": peak_bytes is not None and peak_bytes <= limit_bytes,
    "end_rss_bytes": int(rss_kb) * 1024 if rss_kb.isdigit() else None,
    "gpu_estimate": gpu_estimate.strip(),
    "peak_vram_mib": vram_mib,
    "peak_vram_scope": "nvidia-smi memory.used 全卡口径（含桌面/WSL 其他占用）",
    "peak_vram_within_limit": vram_mib is not None and vram_mib <= float(limit_gib) * 1024,
    "shutdown": shutdown.strip(),
    "over_budget_probe": {
        "command": "ctx-tokens=131072 max-concurrency=16",
        "exit_code": int(over_code),
        "expected_exit_code": 3,
        "estimate": over_estimate.strip(),
        "passed": int(over_code) == 3,
    },
}
with open(out, "w", encoding="utf-8") as handle:
    json.dump(payload, handle, ensure_ascii=False, indent=2)
print(json.dumps(payload, ensure_ascii=False, indent=2))
PY
