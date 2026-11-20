#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""test_dev.py — tools/dev.py 工具链自检（临时目录 + 伪造输出，不依赖真实编译）。

直接运行：python tools/tests/test_dev.py
覆盖：注册表发现、--json 契约、退出码、无 CMakeLists 降级、clean dry-run/删除保护、
sync 幂等与 config.toml 保护、ninja 进度解析、错误摘录、诊断规则、probe 降级。
"""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS_DIR))

import dev  # noqa: E402
from dev_commands import common  # noqa: E402

EXPECTED_COMMANDS = ["env", "sync", "build", "run", "test", "debug", "diagnose", "clean"]


class DevCliTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self._old_root = os.environ.get("LLMOPT_ROOT")
        os.environ["LLMOPT_ROOT"] = str(self.root)

    def tearDown(self) -> None:
        if self._old_root is None:
            os.environ.pop("LLMOPT_ROOT", None)
        else:
            os.environ["LLMOPT_ROOT"] = self._old_root
        self._tmp.cleanup()

    def run_cli(self, argv: list[str]) -> tuple[int, str]:
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            try:
                rc = dev.main(argv)
            except SystemExit as exc:
                rc = int(exc.code)
        return rc, buf.getvalue()

    # —— 注册表与契约 —— #

    def test_list_lists_all_commands(self) -> None:
        rc, out = self.run_cli(["--list"])
        self.assertEqual(rc, common.EXIT_OK)
        for name in EXPECTED_COMMANDS:
            self.assertIn(name, out)

    def test_all_commands_support_json(self) -> None:
        commands = dev.discover_commands()
        parser = dev.build_parser(commands)
        sub_action = next(
            a for a in parser._actions
            if isinstance(a, argparse._SubParsersAction)
        )
        for name, sub_parser in sub_action.choices.items():
            opts = {opt for action in sub_parser._actions for opt in action.option_strings}
            self.assertIn("--json", opts, f"命令 {name} 缺少 --json")

    def test_unknown_command_exit_arg_error(self) -> None:
        rc, _ = self.run_cli(["no-such-command"])
        self.assertEqual(rc, common.EXIT_ARG_ERROR)

    def test_exit_codes_distinct(self) -> None:
        codes = {common.EXIT_OK, common.EXIT_TEST_FAIL, common.EXIT_ENV_UNAVAILABLE,
                 common.EXIT_ARG_ERROR, common.EXIT_BUILD_FAIL}
        self.assertEqual(len(codes), 5)

    # —— build 优雅降级 —— #

    def test_build_skip_without_cmakelists(self) -> None:
        rc, out = self.run_cli(["build"])
        self.assertEqual(rc, common.EXIT_OK)
        self.assertIn("未找到 CMakeLists.txt", out)

    def test_ninja_progress_parse(self) -> None:
        from dev_commands.build_cmd import parse_progress, extract_error

        good = parse_progress("[12/100] Building CXX object foo.cpp")
        self.assertIsNotNone(good)
        self.assertEqual(good["cur"], 12)
        self.assertEqual(good["total"], 100)
        self.assertAlmostEqual(good["percent"], 12.0)
        self.assertIn("foo.cpp", good["target"])
        self.assertIsNone(parse_progress("cmake configuration done"))

        lines = ["configure", "build", "error: something broke", "ctx1", "ctx2"]
        errors = extract_error(lines, context=10)
        self.assertEqual(errors[0], "error: something broke")
        self.assertEqual(len(errors), 3)

    # —— clean dry-run 与保护 —— #

    def _make_clean_fixture(self) -> None:
        (self.root / "build").mkdir()
        (self.root / "build" / "obj.o").write_text("x", encoding="utf-8")
        (self.root / "logs").mkdir()
        (self.root / "logs" / "a.log").write_text("y", encoding="utf-8")
        (self.root / "models").mkdir()
        (self.root / "models" / "m.bin").write_text("z", encoding="utf-8")
        (self.root / "vendor").mkdir()
        (self.root / "vendor" / "v.txt").write_text("w", encoding="utf-8")

    def test_clean_dry_run_deletes_nothing(self) -> None:
        self._make_clean_fixture()
        rc, out = self.run_cli(["clean", "--all"])
        self.assertEqual(rc, common.EXIT_OK)
        self.assertIn("dry-run", out)
        self.assertTrue((self.root / "build" / "obj.o").exists())
        self.assertTrue((self.root / "logs" / "a.log").exists())
        self.assertTrue((self.root / "models" / "m.bin").exists())
        self.assertTrue((self.root / "vendor" / "v.txt").exists())

    def test_clean_yes_keeps_vendor_and_models(self) -> None:
        self._make_clean_fixture()
        rc, _ = self.run_cli(["clean", "--all", "--yes"])
        self.assertEqual(rc, common.EXIT_OK)
        self.assertFalse((self.root / "build" / "obj.o").exists())
        self.assertFalse((self.root / "logs" / "a.log").exists())
        self.assertTrue((self.root / "models" / "m.bin").exists())
        self.assertTrue((self.root / "vendor" / "v.txt").exists())

    # —— sync 幂等与 config 保护 —— #

    def test_sync_idempotent_and_config_protected(self) -> None:
        example = self.root / "config.example.toml"
        example.write_text("port = 8080\n", encoding="utf-8")

        rc1, out1 = self.run_cli(["sync"])
        self.assertEqual(rc1, common.EXIT_OK)
        config = self.root / "config.toml"
        self.assertTrue(config.exists())
        self.assertEqual(config.read_text(encoding="utf-8"), "port = 8080\n")
        manifest = self.root / "vendor" / "manifest.json"
        self.assertTrue(manifest.exists())

        # 改动 config.toml 后再次 sync，不得覆盖
        config.write_text("port = 9999\n", encoding="utf-8")
        manifest_before = manifest.read_text(encoding="utf-8")
        rc2, out2 = self.run_cli(["sync"])
        self.assertEqual(rc2, common.EXIT_OK)
        self.assertEqual(config.read_text(encoding="utf-8"), "port = 9999\n")
        self.assertEqual(manifest.read_text(encoding="utf-8"), manifest_before)
        self.assertIn("不覆盖", out2)

    def test_sync_no_example_keeps_no_config(self) -> None:
        rc, _ = self.run_cli(["sync"])
        self.assertEqual(rc, common.EXIT_OK)
        self.assertFalse((self.root / "config.toml").exists())

    # —— 诊断规则 —— #

    def test_diagnose_matches_rules(self) -> None:
        server = self.root / "logs" / "server"
        server.mkdir(parents=True)
        (server / "app.log").write_text(
            "CUDA out of memory: failed to allocate 1024 MiB\n"
            "step took 120 ms\n"
            "KV alloc/free count mismatch: alloc=5 free=3\n",
            encoding="utf-8",
        )
        rc, out = self.run_cli(["diagnose", "--json"])
        self.assertEqual(rc, common.EXIT_OK)
        payload = json.loads(out)
        ids = {f["rule"] for f in payload["findings"]}
        self.assertIn("oom", ids)
        self.assertIn("slow_step", ids)
        self.assertIn("kv_mismatch", ids)

    # —— 降级路径 —— #

    def test_test_skip_without_targets(self) -> None:
        rc, out = self.run_cli(["test"])
        self.assertEqual(rc, common.EXIT_OK)
        self.assertIn("跳过测试", out)

    def test_run_missing_binary_exit_env(self) -> None:
        rc, _ = self.run_cli(["run"])
        self.assertEqual(rc, common.EXIT_ENV_UNAVAILABLE)

    def test_debug_missing_binary_exit_env(self) -> None:
        rc, _ = self.run_cli(["debug", "--bin", "nonexistent.bin"])
        self.assertEqual(rc, common.EXIT_ENV_UNAVAILABLE)

    def test_env_report_structure(self) -> None:
        rc, out = self.run_cli(["env", "--json"])
        self.assertIn(rc, (common.EXIT_OK, common.EXIT_ENV_UNAVAILABLE))
        payload = json.loads(out)
        for key in ("probe", "toolchain", "vendor", "notes"):
            self.assertIn(key, payload)
        self.assertIn("cmake", payload["toolchain"])
        self.assertIn("ninja", payload["toolchain"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
