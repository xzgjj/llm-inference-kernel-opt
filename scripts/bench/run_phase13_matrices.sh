#!/usr/bin/env bash
# D2-03 四组消融矩阵：同机同模型、每组 5 轮，产物落 evidence/phase13/<name>/。
# 顺序：static（补位净收益）→ mixed（判 1.3×）→ prefix_on / prefix_off（前缀命中收益）。
#
# 用法（WSL 内，仓库根）：
#   bash scripts/bench/run_phase13_matrices.sh [repo_root]
# 覆盖默认路径：
#   LLMOPT_SERVER=/root/build-llmopt/llmopt_server LLMOPT_MODEL=/path/to/model.gguf bash ...
set -uo pipefail

ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
SERVER="${LLMOPT_SERVER:-/root/build-llmopt/llmopt_server}"
MODEL="${LLMOPT_MODEL:-/mnt/d/ai_model/models/qwen3-8b/Qwen3-8B-Q3_K_M.gguf}"

if [[ ! -x "$SERVER" ]]; then
  echo "[d203] server 不存在或不可执行: $SERVER" >&2
  exit 3
fi
if [[ ! -f "$MODEL" ]]; then
  echo "[d203] 模型不存在: $MODEL" >&2
  exit 3
fi

failed=0
for cfg in static mixed prefix_on prefix_off; do
  name="perf-${cfg//_/-}"
  out="$ROOT/evidence/phase13/$name"
  mkdir -p "$out"
  echo "[d203] $name 开始：$(date -Is)"
  python3 scripts/bench/matrix_runner.py \
    --config "scripts/bench/matrix/phase12_s3_${cfg}.json" \
    --out "$out" --run-id "d203-$name" \
    --server "$SERVER" --model "$MODEL" \
    >> "$out/run.log" 2>&1
  rc=$?
  echo "[d203] $name rc=$rc 结束：$(date -Is)"
  [[ $rc -ne 0 ]] && failed=$rc
done

exit "$failed"
