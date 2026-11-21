#!/usr/bin/env bash
# 阶段 12 S5：容器冒烟五项 + drain 退出码。
#   1 healthz 200
#   2 readyz 200 且 body 含 ready
#   3 非流式 /v1/completions 200 且 body 含 object
#   4 SSE stream=true 收到 >=2 个 data: 帧
#   5 断连取消（仅当二进制支持 --disconnect-poll-ms；否则 SKIP 并写原因）
# 另断言 docker stop 退出码 0（drain 收敛）。
# 结果写 evidence/phase12/packaging/docker_smoke.json：每项 status=PASS|SKIP|FAIL + 证据片段。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="${IMAGE:-llmopt:s5}"
NAME="${NAME:-llmopt-smoke}"
PORT="${PORT:-18080}"
WORK="$(mktemp -d)"
EVIDENCE="${EVIDENCE:-$ROOT/evidence/phase12/packaging}"
mkdir -p "$EVIDENCE"
MANIFEST="$WORK/result.tsv"
: > "$MANIFEST"

cleanup() {
  docker rm -f "$NAME" >/dev/null 2>&1 || true
  rm -rf "$WORK"
}
trap cleanup EXIT

record() { # name status evidence
  printf '%s\t%s\t%s\n' "$1" "$2" "$3" >> "$MANIFEST"
}

# ---- 伪造最小 GGUF（复用 server_kit 口径，不依赖真实权重）----
python3 - "$WORK/smoke.gguf" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, "tests/integration")
import server_kit as kit
kit.make_gguf(Path(sys.argv[1]))
PY
# 容器以 uid 10001 运行：源码端临时目录/文件需对其他用户可读（WSL mktemp 默认 0700）。
chmod 0755 "$WORK"
chmod 0644 "$WORK/smoke.gguf"

echo "[smoke] building image $IMAGE"
docker build -f scripts/release/Dockerfile -t "$IMAGE" "$ROOT"

echo "[smoke] starting container on port $PORT"
docker run -d --name "$NAME" -p "$PORT:8080" \
  -e LLMOPT_ALLOW_STUB=1 \
  -e LLMOPT_TEST_STUB_DELAY_MS=50 \
  -v "$WORK:/app/models:ro" \
  "$IMAGE" \
  --model /app/models/smoke.gguf --host 0.0.0.0 --port 8080 \
  --control-port 8081 --backend stub --mode off --log-level warn

# 等就绪（最多 20s）
ready=0
for _ in $(seq 1 100); do
  if curl -fsS "http://127.0.0.1:$PORT/readyz" >/dev/null 2>&1; then ready=1; break; fi
  sleep 0.2
done
if [ "$ready" != "1" ]; then
  echo "[smoke] container never became ready; logs:"
  docker logs "$NAME" 2>&1 | tail -40
fi

# ---- 1 healthz ----
body_hz=$(curl -s -w '\n%{http_code}' "http://127.0.0.1:$PORT/healthz")
code_hz=$(echo "$body_hz" | tail -1)
if [ "$code_hz" = "200" ]; then
  record healthz PASS "HTTP $code_hz $(echo "$body_hz" | head -1)"
else
  record healthz FAIL "HTTP $code_hz body=$(echo "$body_hz" | head -1)"
fi

# ---- 2 readyz ----
body_rz=$(curl -s -w '\n%{http_code}' "http://127.0.0.1:$PORT/readyz")
code_rz=$(echo "$body_rz" | tail -1)
if [ "$code_rz" = "200" ] && echo "$body_rz" | grep -q '"ready"'; then
  record readyz PASS "HTTP $code_rz $(echo "$body_rz" | head -1)"
else
  record readyz FAIL "HTTP $code_rz body=$(echo "$body_rz" | head -1)"
fi

# ---- 3 非流式 completions ----
body_ns=$(curl -s -w '\n%{http_code}' -H 'Content-Type: application/json' \
  -d '{"prompt":"smoke-nonstream","max_tokens":4}' \
  "http://127.0.0.1:$PORT/v1/completions")
code_ns=$(echo "$body_ns" | tail -1)
if [ "$code_ns" = "200" ] && echo "$body_ns" | grep -q '"object"'; then
  record completions_nonstream PASS "HTTP $code_ns body=$(echo "$body_ns" | head -1 | cut -c1-160)"
else
  record completions_nonstream FAIL "HTTP $code_ns body=$(echo "$body_ns" | head -1 | cut -c1-160)"
fi

# ---- 4 SSE stream ----
body_sse=$(curl -s -N -H 'Content-Type: application/json' \
  -d '{"prompt":"smoke-sse","max_tokens":4,"stream":true}' \
  "http://127.0.0.1:$PORT/v1/completions")
frames=$(echo "$body_sse" | grep -c '^data: ' || true)
if [ "$frames" -ge 2 ]; then
  record completions_sse PASS "data frames=$frames; first=$(echo "$body_sse" | grep '^data: ' | head -1 | cut -c1-160)"
else
  record completions_sse FAIL "data frames=$frames; body=$(echo "$body_sse" | head -3 | cut -c1-160)"
fi

# ---- 5 断连取消 ----
cancelled_now() {
  curl -s "http://127.0.0.1:$PORT/metrics" \
    | grep 'llmopt_requests_total{status="cancelled"}' || echo 'llmopt_requests_total{status="cancelled"} 0'
}
# S1 断连轮询为内置行为（http_server.cpp 内 500ms is_connection_closed 轮询）；
# CLI 未暴露 --disconnect-poll-ms 开关，故不以该 flag 作为能力判据，直接裸 socket 真跑。
baseline=$(cancelled_now | awk '{print $2}')
# 裸 socket 发请求后硬断开（RST），3s 内观察 cancelled 计数 +1。
python3 - "$PORT" <<'PY'
import socket, sys, time, json
port = int(sys.argv[1])
body = json.dumps({"prompt":"disconnect-smoke","max_tokens":600,"temperature":0}).encode()
sock = socket.create_connection(("127.0.0.1", port), timeout=10)
req = (b"POST /v1/completions HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
       b"Content-Length: " + str(len(body)).encode() + b"\r\nConnection: close\r\n\r\n" + body)
sock.sendall(req)
time.sleep(0.8)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")
sock.close()
PY
bumped=0
for _ in $(seq 1 12); do
  sleep 0.25
  current=$(cancelled_now | awk '{print $2}')
  if [ "${current:-0}" -gt "${baseline:-0}" ]; then bumped=1; break; fi
done
if [ "$bumped" = "1" ]; then
  record disconnect_cancel PASS "cancelled ${baseline:-0} -> ${current:-0} within 3s"
else
  record disconnect_cancel FAIL "cancelled 未增长 baseline=${baseline:-0} current=${current:-0}"
fi

# ---- drain：SIGTERM 后退出码 0 ----
docker stop --time 40 "$NAME" >/dev/null
stop_code=$(docker inspect -f '{{.State.ExitCode}}' "$NAME")
if [ "$stop_code" = "0" ]; then
  record drain_stop PASS "docker stop exit=$stop_code"
else
  record drain_stop FAIL "docker stop exit=$stop_code"
fi

# ---- 汇总 JSON ----
python3 - "$MANIFEST" "$EVIDENCE/docker_smoke.json" <<'PY'
import json, sys
from pathlib import Path
manifest, out = sys.argv[1], sys.argv[2]
items = []
for line in Path(manifest).read_text().splitlines():
    if not line.strip():
        continue
    name, status, evidence = line.split("\t", 2)
    items.append({"name": name, "status": status, "evidence": evidence})
doc = {"image": "llmopt:s5", "items": items,
       "summary": {s: sum(i["status"] == s for i in items) for s in ("PASS", "SKIP", "FAIL")}}
Path(out).write_text(json.dumps(doc, ensure_ascii=False, indent=2) + "\n")
print(json.dumps(doc["summary"], ensure_ascii=False))
PY

echo "[smoke] evidence -> $EVIDENCE/docker_smoke.json"
