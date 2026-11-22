#!/usr/bin/env bash
# D2-07 六项冒烟：真后端镜像（CP 版，ngl 0）。
# 用法：bash scripts/release/smoke_image.sh <image:tag> <model_dir> <model_file>
set -uo pipefail
image="${1:-llmopt:0.13.0-llama}"
model_dir="${2:-/mnt/d/ai_model/models/qwen3-8b}"
model_file="${3:-Qwen3-8B-Q3_K_M.gguf}"
name=llmopt-smoke
port=18080
base="http://127.0.0.1:${port}"
fail=0

pass() { echo "PASS $1"; }
bad() { echo "FAIL $1"; fail=1; }

docker rm -f "$name" >/dev/null 2>&1 || true
docker run -d --name "$name" -p "${port}:8080" \
    -v "${model_dir}:/app/models:ro" \
    "$image" --host 0.0.0.0 --port 8080 --backend llama \
    --model "/app/models/${model_file}" --n-gpu-layers 0 --threads 12 --ctx-tokens 1024 \
    >/dev/null || { echo "FAIL docker run"; exit 1; }

for _ in $(seq 1 60); do
    code=$(curl -s -o /dev/null -w '%{http_code}' "${base}/readyz" || true)
    [ "$code" = "200" ] && break
    sleep 2
done

code=$(curl -s -o /dev/null -w '%{http_code}' "${base}/healthz"); [ "$code" = "200" ] && pass "healthz=200" || bad "healthz=${code}"
code=$(curl -s -o /dev/null -w '%{http_code}' "${base}/readyz"); [ "$code" = "200" ] && pass "readyz=200" || bad "readyz=${code}"

metrics=$(curl -s "${base}/metrics")
echo "$metrics" | grep -q 'llmopt_ttft_ms' && echo "$metrics" | grep -q 'llmopt_itl_ms' \
    && pass "metrics 三支直方图" || bad "metrics 缺直方图"

body=$(curl -s -X POST "${base}/completion" -H 'content-type: application/json' \
    -d '{"prompt":"你好","max_tokens":8,"seed":7}')
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "${base}/completion" -H 'content-type: application/json' \
    -d '{"prompt":"再试一次","max_tokens":8,"seed":9}')
echo "$body" | grep -q '"choices"' && [ "$code" = "200" ] && pass "completion 200 + choices" || bad "completion=${code}"

uid=$(docker exec "$name" id -u); [ "$uid" = "10001" ] && pass "非 root uid=10001" || bad "uid=${uid}"

docker stop "$name" >/dev/null 2>&1; stop_rc=$?
docker inspect -f '{{.State.ExitCode}}' "$name" >/tmp/smoke-exit 2>/dev/null || true
ec=$(cat /tmp/smoke-exit 2>/dev/null || echo "?")
[ "$stop_rc" = "0" ] && [ "$ec" = "0" ] && pass "docker stop 退出码 0" || bad "docker stop rc=${stop_rc} container=${ec}"

docker rm -f "$name" >/dev/null 2>&1 || true
echo "SMOKE_RESULT=$([ "$fail" = "0" ] && echo PASS || echo FAIL)"
exit "$fail"
