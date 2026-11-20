#!/usr/bin/env bash
# 6-G6：镜像起服务 → /readyz 200 → 一条 chat 成功 → SIGTERM 退出码 0。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="${IMAGE:-llmopt:smoke}"
NAME="${NAME:-llmopt-smoke}"
WORK="$(mktemp -d)"
cleanup() {
  docker rm -f "$NAME" >/dev/null 2>&1 || true
  rm -rf "$WORK"
}
trap cleanup EXIT

python3 - "$WORK/smoke.gguf" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, "tests/integration")
import server_kit as kit
kit.make_gguf(Path(sys.argv[1]))
PY

docker build -f scripts/release/Dockerfile -t "$IMAGE" "$ROOT"
docker run -d --name "$NAME" -p 18080:8080 -e LLMOPT_ALLOW_STUB=1 \
  -v "$WORK:/data:ro" "$IMAGE" \
  --model /data/smoke.gguf --host 0.0.0.0 --port 8080 --backend stub --mode off

for _ in $(seq 1 100); do
  if curl -fsS http://127.0.0.1:18080/readyz >/dev/null 2>&1; then break; fi
  sleep 0.2
done
curl -fsS http://127.0.0.1:18080/readyz | grep -q '"ready"'
curl -fsS http://127.0.0.1:18080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"smoke","messages":[{"role":"user","content":"hi"}],"max_tokens":4}' \
  | grep -q '"object":"chat.completion"'

docker stop --time 40 "$NAME" >/dev/null
code="$(docker inspect -f '{{.State.ExitCode}}' "$NAME")"
test "$code" = "0"
echo "deploy smoke ok: exit=$code"
