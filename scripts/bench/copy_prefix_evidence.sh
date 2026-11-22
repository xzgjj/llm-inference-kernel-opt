#!/usr/bin/env bash
# 把 WSL 里重跑后的 prefix 证据整目录同步到 Windows 仓库（旧数据不混进来）。
set -euo pipefail
src=/root/work/llmopt/evidence/phase13
dst=/mnt/d/c++_myproject/llm-inference-kernel-opt/evidence/phase13
for g in perf-prefix-on perf-prefix-off; do
  rm -rf "$dst/$g"
  cp -r "$src/$g" "$dst/$g"
done
du -sh "$dst/perf-prefix-on" "$dst/perf-prefix-off"
