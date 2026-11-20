"""sync — 随项目更新。

- 校验 vendor/manifest.json（name/version/sha256/source）；无清单时创建模板，不报错
- 生成/刷新 build/CMakePresets.json（dev/debug/release/asan）
- 建目录骨架 build/ logs/ models/
- config.toml 仅在缺失时从 config.example.toml 复制，已存在绝不覆盖
- git commit 与依赖版本写入 build/env.json
"""

from __future__ import annotations

import json
import shutil
from pathlib import Path

from dev_commands import common

NAME = "sync"
HELP = "清单校验/目录骨架/CMakePresets/config.toml 保护式刷新，写 build/env.json"

MANIFEST_TEMPLATE = {
    "schema_version": "1.0",
    "note": "vendor 依赖清单模板；条目必填 name/version/sha256/source。",
    "entries": [],
}

PRESETS = {
    "version": 3,
    "cmakeMinimumRequired": {"major": 3, "minor": 20, "patch": 0},
    "configurePresets": [
        {
            "name": "dev",
            "displayName": "Dev (RelWithDebInfo)",
            "binaryDir": "${sourceDir}/build/dev",
            "cacheVariables": {"CMAKE_BUILD_TYPE": "RelWithDebInfo"},
        },
        {
            "name": "debug",
            "displayName": "Debug",
            "binaryDir": "${sourceDir}/build/debug",
            "cacheVariables": {"CMAKE_BUILD_TYPE": "Debug"},
        },
        {
            "name": "release",
            "displayName": "Release",
            "binaryDir": "${sourceDir}/build/release",
            "cacheVariables": {"CMAKE_BUILD_TYPE": "Release"},
        },
        {
            "name": "asan",
            "displayName": "ASan (Debug + AddressSanitizer)",
            "binaryDir": "${sourceDir}/build/asan",
            "cacheVariables": {
                "CMAKE_BUILD_TYPE": "Debug",
                "CMAKE_CXX_FLAGS": "-fsanitize=address -g",
                "CMAKE_EXE_LINKER_FLAGS": "-fsanitize=address",
            },
        },
    ],
}


def _git_commit(root: Path) -> str | None:
    rc, out, _ = common.run_cmd(["git", "-C", str(root), "rev-parse", "HEAD"], timeout=5)
    return out if rc == 0 and out else None


def _validate_manifest(root: Path) -> list[str]:
    """校验既有清单；返回问题清单（缺失/字段不符），不做网络拉取。"""
    issues: list[str] = []
    status = common.vendor_status(root)
    if status["manifest"] != "present":
        issues.append(status.get("error") or "清单不可读")
        return issues
    for entry in status["entries"]:
        if not entry["fields_ok"]:
            issues.append(f"条目 {entry.get('name')!r} 缺字段（需 name/version/sha256/source）")
        if not entry["present"]:
            issues.append(f"依赖目录缺失: vendor/{entry.get('name')}")
    return issues


def add_arguments(parser) -> None:
    parser.add_argument("--vendor", action="store_true", help="校验 vendor 条目（默认仅体检）")


def run(args) -> int:
    root = common.repo_root()
    actions: list[str] = []
    warnings: list[str] = []

    # 1. 目录骨架
    for directory in ("build", "logs", "models", "build/logs"):
        (root / directory).mkdir(parents=True, exist_ok=True)
    actions.append("目录骨架就绪: build/ logs/ models/")

    # 2. vendor 清单：缺失则建模板，存在则校验
    manifest = root / "vendor" / "manifest.json"
    if not manifest.exists():
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest.write_text(
            json.dumps(MANIFEST_TEMPLATE, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        actions.append("无 vendor/manifest.json，已创建模板（未报错）")
    else:
        issues = _validate_manifest(root)
        if issues:
            warnings.extend(issues)
        else:
            actions.append("vendor/manifest.json 校验通过")

    # 3. CMakePresets（产物落 build/，不写源码目录）
    presets_path = root / "build" / "CMakePresets.json"
    presets_path.write_text(
        json.dumps(PRESETS, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    actions.append(f"刷新 {presets_path.relative_to(root)}")

    # 4. config.toml：已存在绝不覆盖
    config = root / "config.toml"
    example = root / "config.example.toml"
    if config.exists():
        actions.append("config.toml 已存在，跳过（不覆盖）")
    elif example.exists():
        shutil.copy2(example, config)
        actions.append("从 config.example.toml 复制 config.toml")
    else:
        actions.append("无 config.example.toml，跳过 config.toml")

    # 5. git commit + 依赖版本写入 build/env.json（保留既有报告其它字段）
    env_path = root / "build" / "env.json"
    env_data: dict = {}
    if env_path.exists():
        try:
            env_data = json.loads(env_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            warnings.append("既有 build/env.json 损坏，sync 段重建")
            env_data = {}
    vendor = common.vendor_status(root)
    env_data["sync"] = {
        "synced_at": common.now_iso(),
        "git_commit": _git_commit(root),
        "dependencies": [
            {"name": e.get("name"), "version": e.get("version"), "source": e.get("source")}
            for e in vendor.get("entries", [])
        ],
    }
    env_path.parent.mkdir(parents=True, exist_ok=True)
    env_path.write_text(json.dumps(env_data, ensure_ascii=False, indent=2), encoding="utf-8")
    actions.append("git commit 与依赖版本已写入 build/env.json")

    if getattr(args, "json", False):
        common.print_json({"actions": actions, "warnings": warnings})
    else:
        for line in actions:
            print(f"[sync] {line}")
        for warn in warnings:
            print(f"[sync] warn: {warn}")
    return common.EXIT_OK
