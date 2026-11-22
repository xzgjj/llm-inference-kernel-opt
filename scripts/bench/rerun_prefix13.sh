#!/usr/bin/env bash
# 只重跑 prefix 两组（D2-03 修复 seq_cp 前缀区间后复测）；旧的坏数据保留为 *-pre-fix。
set -uo pipefail
ROOT="${1:-/root/work/llmopt}"
SERVER="${LLMOPT_SERVER:-/root/build-llmopt/llmopt_server}"
MODEL="${LLMOPT_MODEL:-/mnt/d/ai_model/models/qwen3-8b/Qwen3-8B-Q3_K_M.gguf}"

cd "$ROOT" || exit 3
for g in prefix_on prefix_off; do
  name="perf-${g//_/-}"
  if [[ -d "evidence/phase13/$name" ]]; then
    rm -rf "evidence/phase13/$name-pre-fix"
    mv "evidence/phase13/$name" "evidence/phase13/$name-pre-fix"
  fi
  mkdir -p "evidence/phase13/$name"
  echo "[d203] $name 开始：$(date -Is)"
  python3 scripts/bench/matrix_runner.py \
    --config "scripts/bench/matrix/phase12_s3_${g}.json" \
    --out "evidence/phase13/$name" --run-id "d203-$name" \
    --server "$SERVER" --model "$MODEL" \
    >> "evidence/phase13/$name/run.log" 2>&1
  echo "[d203] $name rc=$? 结束：$(date -Is)"
done
