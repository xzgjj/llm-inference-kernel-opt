#!/usr/bin/env bash
# repro.sh — 阶段 4 矩阵一键复现入口（4-G1）。
# 用法：
#   scripts/repro.sh scripts/bench/matrix/phase4.json [out_dir]
#   LLMOPT_SERVER_BIN=build/llmopt_server LLMOPT_MODEL=/root/models/q.gguf \
#     LLMOPT_BACKEND=llama scripts/repro.sh scripts/bench/matrix/phase4.json
# 说明：SPEC 写的是 phase4.yaml；项目不引 YAML 库，配置已改为 JSON，记录在实现记录。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${1:-${ROOT}/scripts/bench/matrix/phase4.json}"
OUT="${2:-${ROOT}/bench/results/phase4/$(date -u +p4-%Y%m%dT%H%M%SZ)}"
SERVER="${LLMOPT_SERVER_BIN:-${ROOT}/build/llmopt_server}"
MODEL="${LLMOPT_MODEL:-${ROOT}/bench/models/qwen2.5-0.5b-q4.gguf}"
BACKEND="${LLMOPT_BACKEND:-llama}"

if [[ ! -f "${SERVER}" ]]; then
    echo "server 不存在：${SERVER}（先构建，或设 LLMOPT_SERVER_BIN）" >&2
    exit 2
fi
if [[ ! -f "${MODEL}" ]]; then
    echo "模型不存在：${MODEL}（设 LLMOPT_MODEL）" >&2
    exit 2
fi

cd "${ROOT}"
exec python3 scripts/bench/matrix_runner.py \
    --config "${CONFIG}" \
    --out "${OUT}" \
    --server "${SERVER}" \
    --model "${MODEL}" \
    --backend "${BACKEND}"
