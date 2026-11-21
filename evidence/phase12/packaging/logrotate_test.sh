#!/usr/bin/env bash
# 阶段 12 S5：logrotate 配置实测容器脚本（ubuntu:24.04 内执行）。
# 1) apt 装 logrotate；2) logrotate -d 语法校验；3) 造 21MB 假日志，
#    logrotate -f 强转；4) 断言生成 .1 且原文件被截断到 <1MiB。
set -euo pipefail

apt-get update -qq
apt-get install -y -qq logrotate

id llmopt 2>/dev/null || useradd --system llmopt
mkdir -p /var/log/llmopt
chown llmopt:llmopt /var/log/llmopt

dd if=/dev/zero of=/var/log/llmopt/llmopt.log bs=1M count=21 status=none
chown llmopt:llmopt /var/log/llmopt/llmopt.log

echo "== before =="
ls -l /var/log/llmopt

echo "== logrotate -d (debug / syntax check) =="
logrotate -d /etc/logrotate.d/llmopt

echo "== logrotate -f (force) =="
logrotate -f /etc/logrotate.d/llmopt

echo "== after =="
ls -l /var/log/llmopt

test -f /var/log/llmopt/llmopt.log.1
orig=$(stat -c%s /var/log/llmopt/llmopt.log)
rot=$(stat -c%s /var/log/llmopt/llmopt.log.1)
echo "original truncated size=${orig} bytes; rotated .1 size=${rot} bytes"
test "${orig}" -lt 1048576
echo "LOGROTATE_OK"
