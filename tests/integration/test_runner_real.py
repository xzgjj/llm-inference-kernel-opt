"""真模型 runner 进程级探针：缺 LLMOPT_TEST_MODEL 时以 77 跳过，不伪装成通过。"""

from __future__ import annotations

import os
import subprocess
import sys


def main() -> int:
    model = os.environ.get("LLMOPT_TEST_MODEL", "")
    if not model:
        print("SKIP: 未设置 LLMOPT_TEST_MODEL，真模型 load/tokenize/reload 路径不执行")
        return 77
    if len(sys.argv) != 2:
        print("用法: test_runner_real.py <runner_real_probe>")
        return 2
    probe = sys.argv[1]
    try:
        result = subprocess.run([probe, model], check=False, timeout=300)
    except (OSError, subprocess.TimeoutExpired) as exc:
        print(f"FAIL: 探针无法完成: {exc}")
        return 1
    if result.returncode != 0:
        print(f"FAIL: 探针退出码 {result.returncode}")
        return result.returncode or 1
    print(f"PASS: 真模型 load->tokenize->release->reload 通过（{model}）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
