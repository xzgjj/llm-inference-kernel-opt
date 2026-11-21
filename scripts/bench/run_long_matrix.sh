#!/usr/bin/env bash
# 长 prompt 矩阵（阶段 8 缩减档）：关前缀缓存、1400 字符档（≈522 真实 token）。
# 同时启动进度监控，避免长时间 prefill 被误判为卡死。
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT=bench/results/phase8b_long
LOG="$OUT/progress.log"
mkdir -p "$OUT"

python3 scripts/bench/watch_matrix.py --out "$OUT" --log "$LOG" \
  --interval 10 --until-report &
WATCH_PID=$!
trap 'kill "$WATCH_PID" 2>/dev/null || true' EXIT

python3 scripts/bench/matrix_runner.py \
  --config scripts/bench/matrix/phase8b_long.json \
  --out "$OUT"

wait "$WATCH_PID" || true
