# test_crash_dump.py — S5 崩溃回溯：SIGSEGV 后落 dump（signal/pid/version），非零退出，保留 2 份。
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    fixture = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        result = subprocess.run([str(fixture)], cwd=str(workdir),
                                capture_output=True, text=True, timeout=60)
        assert result.returncode != 0, \
            f"崩溃 fixture 应以非零码退出，实际 {result.returncode}"
        dumps = sorted((workdir / "logs" / "crash").glob("crash-*.txt"))
        assert dumps, f"未生成 crash dump：{list(workdir.rglob('*'))}"
        text = dumps[-1].read_text(encoding="utf-8", errors="replace")
        for field in ("signal=", "pid=", "version="):
            assert field in text, f"dump 缺少 {field} 字段：\n{text[:400]}"
        assert len(dumps) <= 2, f"dump 保留数应 ≤2，实际 {len(dumps)}"

        for _ in range(3):
            subprocess.run([str(fixture)], cwd=str(workdir),
                           capture_output=True, text=True, timeout=60)
        dumps = sorted((workdir / "logs" / "crash").glob("crash-*.txt"))
        assert len(dumps) == 2, f"连续崩溃后应只保留 2 份 dump，实际 {len(dumps)}"
        print(f"OK crash_dump retention={len(dumps)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
