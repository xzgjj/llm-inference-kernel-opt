#!/usr/bin/env bash
# 真服务 e2e 的统一入口：没有 LLMOPT_REAL_BASE 时以 77 跳过，不伪装通过。
set -euo pipefail
cd "$(dirname "$0")/../.."

if [[ -z "${LLMOPT_REAL_BASE:-}" ]]; then
    echo "SKIP: 未设置 LLMOPT_REAL_BASE，真实服务契约不执行" >&2
    exit 77
fi

exec python3 tests/e2e/test_real_service_contract.py
