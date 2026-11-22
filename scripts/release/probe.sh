#!/usr/bin/env bash
# probe.sh — D2-11 外部存活探针：连续 3 次抓 /readyz，任一次非 200 就打一条告警行。
#
# 设计：进程内 /readyz 只说明自己还活着，探针从外部再确认一次，供 cron 或
# systemd timer 调用；本脚本不重启服务、不改状态，只观测与落日志。
#
# 用法：
#   scripts/release/probe.sh                       # 默认 http://127.0.0.1:8080/readyz
#   LLMOPT_PROBE_URL=http://127.0.0.1:9090/readyz scripts/release/probe.sh
#   LLMOPT_PROBE_LOG=/var/log/llmopt/probe.log scripts/release/probe.sh
#
# 退出码：0 三次都 200；1 有任一次非 200 或连不上；2 curl 不存在（环境不可用）。
set -uo pipefail

URL="${LLMOPT_PROBE_URL:-http://127.0.0.1:8080/readyz}"
LOG="${LLMOPT_PROBE_LOG:-}"
ATTEMPTS="${LLMOPT_PROBE_ATTEMPTS:-3}"
TIMEOUT="${LLMOPT_PROBE_TIMEOUT:-3}"

if ! command -v curl >/dev/null 2>&1; then
    echo "probe.sh: 需要 curl（未安装）" >&2
    exit 2
fi

failures=0
codes=""
index=1
while [ "${index}" -le "${ATTEMPTS}" ]; do
    code=$(curl -s -o /dev/null -m "${TIMEOUT}" -w '%{http_code}' "${URL}" 2>/dev/null || true)
    [ -z "${code}" ] && code="000"
    codes="${codes}${codes:+ }${code}"
    if [ "${code}" != "200" ]; then
        failures=$((failures + 1))
    fi
    index=$((index + 1))
done

stamp=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
if [ "${failures}" -gt 0 ]; then
    line="ALERT ${stamp} probe url=${URL} codes=[${codes}] failures=${failures}/${ATTEMPTS}"
    echo "${line}" >&2
    if [ -n "${LOG}" ]; then
        mkdir -p "$(dirname "${LOG}")" 2>/dev/null || true
        echo "${line}" >>"${LOG}"
    fi
    exit 1
fi

line="OK ${stamp} probe url=${URL} codes=[${codes}]"
if [ -n "${LOG}" ]; then
    mkdir -p "$(dirname "${LOG}")" 2>/dev/null || true
    echo "${line}" >>"${LOG}"
fi
exit 0
