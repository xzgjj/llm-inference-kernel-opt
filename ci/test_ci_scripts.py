"""CI 工具自测：规则也是代码，也需要测试。

rule: CI-TST-001
"""

from __future__ import annotations

import argparse
import importlib.util
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts" / "ci"))

import check_diff  # noqa: E402
import check_docs  # noqa: E402
import check_tests  # noqa: E402
import ci_common  # noqa: E402
import run_ci  # noqa: E402


def _load_coverage_gate():
    spec = importlib.util.spec_from_file_location(
        "ci_coverage_gate", REPO / "scripts" / "ci" / "check_coverage.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


coverage_gate = _load_coverage_gate()


class WordCountTest(unittest.TestCase):
    def test_chinese_counted_per_char(self) -> None:
        self.assertEqual(ci_common.count_words("文档预算"), 4)

    def test_code_fence_ignored(self) -> None:
        text = "正文\n```bash\npython scripts/ci/run_ci.py --mode fast\n```"
        self.assertEqual(ci_common.count_words(text), 2)

    def test_paths_count_as_one_token(self) -> None:
        self.assertEqual(ci_common.count_words("docs/testing.md"), 1)


class RegistryTest(unittest.TestCase):
    def test_header_and_rows(self) -> None:
        text = ci_common.read_text(REPO / "docs/testing.md")
        self.assertIn("| Scope | Level | Runner | Args | Metadata |", text)
        rows = ci_common.iter_table_rows(text, "Scope")
        self.assertGreaterEqual(len(rows), 1)
        for row in rows:
            self.assertEqual(len(row), 5, f"行列数不符：{row}")

    def test_registry_gate_passes(self) -> None:
        result = check_tests.run()
        self.assertTrue(result.passed, [f"{v.rule}:{v.path}:{v.message}" for v in result.errors])

    def test_every_test_file_registered(self) -> None:
        files = check_tests._test_files()
        self.assertTrue(files, "tests/ 下应至少有一个测试文件")
        result = check_tests.run()
        self.assertFalse(
            [v for v in result.errors if v.rule == "CI-TST-003"],
            "存在未登记的测试文件",
        )


class DocsGateTest(unittest.TestCase):
    def test_docs_gate_passes(self) -> None:
        result = check_docs.run()
        self.assertTrue(result.passed, [f"{v.rule}:{v.path}:{v.message}" for v in result.errors])

    def test_ci_md_within_budget(self) -> None:
        words = ci_common.count_words(ci_common.read_text(REPO / "docs/ci.md"))
        self.assertLessEqual(words, 3000)

    def test_spec_declares_script_rules(self) -> None:
        spec = ci_common.read_text(REPO / "docs/ci/ci-spec.md")
        declared = ci_common.declared_rules(spec)
        self.assertIn("CI-DOC-001", declared)
        self.assertTrue(any(v.startswith("script:") for v in declared.values()))


class CoverageGateTest(unittest.TestCase):
    def test_line_only_counts_src(self) -> None:
        files = {"src/a.cpp": {1: 0, 2: 3, 3: 1}, "include/x.hpp": {1: 0}}
        self.assertEqual(coverage_gate.line(files), (2, 3))

    def test_line_with_changed_set(self) -> None:
        files = {"src/a.cpp": {1: 0, 2: 3}}
        self.assertEqual(coverage_gate.line(files, only={"src/a.cpp:2"}), (1, 1))

    def test_parse_diff_text_ranges(self) -> None:
        text = "--- a/src/a.cpp\n+++ b/src/a.cpp\n@@ -10,0 +11,3 @@\n+x\n+y\n+z\n"
        self.assertEqual(coverage_gate.parse_diff_text(text), {"src/a.cpp": {11, 12, 13}})


class WaiverTest(unittest.TestCase):
    def test_waivers_file_is_valid(self) -> None:
        result = ci_common.CheckResult("waivers")
        waivers = check_diff.load_waivers(result)
        self.assertTrue(result.passed, [f"{v.rule}:{v.path}:{v.message}" for v in result.errors])
        self.assertEqual(waivers, [])


class ScopeWhitelistTest(unittest.TestCase):
    def _violations(self, files: dict[str, str]) -> list[str]:
        result = ci_common.CheckResult("scope")
        args = argparse.Namespace(labels="", enforce=False)
        check_diff.scope_whitelist(result, check_diff.Diff(files=files), args)
        return [v.message for v in result.violations if v.rule == "CI-DIF-001"]

    def test_root_contract_docs_are_allowed(self) -> None:
        # CLI.md 是已入库的 CLI 契约，阶段 5 新增工具章节不能因白名单缺口被判越界。
        self.assertEqual(self._violations({"CLI.md": "M", "README.md": "M"}), [])

    def test_new_top_level_directory_is_rejected(self) -> None:
        self.assertTrue(self._violations({"application/recompute.py": "A"}))

    def test_git_quoted_chinese_path_is_decoded(self) -> None:
        # git 默认把非 ASCII 路径转成 "docs/\346\226\207..."，白名单不能把它当新顶层目录。
        quoted = '"docs/\\346\\226\\207\\346\\241\\243.md"'
        self.assertEqual(check_diff._decode_git_path(quoted), "docs/文档.md")
        self.assertEqual(check_diff._decode_git_path("docs/ascii.md"), "docs/ascii.md")


class EnvCheckTest(unittest.TestCase):
    """D2-02 回归：缺前置工具时入口以 3 退出，而不是让集成用例报 13 条假失败。

    修复前的症状：PATH 里没有 clang-format/git/ctest 时，run_ci 在闸门中途才失败，
    或者 ctest 直接起不来服务（rc=0xC0000135），看起来像代码坏了。
    """

    def test_missing_tool_exits_with_three(self) -> None:
        with mock.patch.object(run_ci.shutil, "which", return_value=None), \
             mock.patch.object(run_ci, "run_step") as step:
            self.assertEqual(run_ci.main(["--mode", "pr"]), 3)
        step.assert_not_called()

    def test_requirements_follow_mode(self) -> None:
        self.assertNotIn("git", run_ci.MODE_REQUIREMENTS["fast"])
        self.assertIn("git", run_ci.MODE_REQUIREMENTS["pr"])
        self.assertIn("ctest", run_ci.MODE_REQUIREMENTS["nightly"])

    def test_all_tools_present_reports_no_missing(self) -> None:
        with mock.patch.object(run_ci.shutil, "which", return_value="/usr/bin/tool"):
            missing, _ = run_ci.env_check("pr")
        self.assertEqual(missing, [])

    def test_docs_count_check_skips_without_ctest(self) -> None:
        """没有 ctest 时计数校验记 SKIP，而不是抛 traceback 判文档违规。"""
        result = ci_common.CheckResult("docs-count")
        with mock.patch.object(check_docs.shutil, "which", return_value=None):
            check_docs.test_counts(result)
        self.assertTrue(any("SKIP" in note for note in result.notes))
        self.assertFalse(any(v.rule == "CI-DOC-008" for v in result.violations))

    def test_windows_missing_ucrt_is_warned(self) -> None:
        with mock.patch.object(run_ci.os, "name", "nt"), \
             mock.patch.dict(run_ci.os.environ, {"PATH": "C:\\Windows\\System32"}):
            _, warnings = run_ci.env_check("fast")
        self.assertTrue(any("ucrt64" in note for note in warnings))


class CliDocTest(unittest.TestCase):
    """D2-14：CLI.md 与实现零漂移。

    两边各自解析：CLI.md 取第 3.1 节参数表的首列，config.cpp 取 kConfigKeys 注册表。
    少写一个参数、写了不存在的参数，都会在 CI 里直接红，不用等人肉 review。
    """

    @staticmethod
    def _registry_keys() -> set[str]:
        text = (REPO / "src" / "config.cpp").read_text(encoding="utf-8")
        block = text.split("kConfigKeys[] = {", 1)[1].split("};", 1)[0]
        return set(re.findall(r'"([a-z0-9-]+)"', block))

    @staticmethod
    def _doc_keys() -> set[str]:
        text = (REPO / "CLI.md").read_text(encoding="utf-8")
        section = text.split("### 3.1", 1)[1].split("### 3.2", 1)[0]
        keys: set[str] = set()
        for line in section.splitlines():
            # 一行可以写多个同义参数（如 --tls-cert / --tls-key），全部收集。
            if line.startswith("| `--"):
                keys.update(re.findall(r"--([a-z0-9-]+)", line))
        return keys

    def test_doc_and_registry_match(self) -> None:
        registry = self._registry_keys()
        self.assertIn("slow-request-ms", registry)
        documented = self._doc_keys()
        self.assertEqual(sorted(registry - documented), [], "CLI.md 漏写参数")
        self.assertEqual(sorted(documented - registry), [], "CLI.md 写了未注册参数")

    def test_dev_commands_documented(self) -> None:
        commands: set[str] = set()
        for path in (REPO / "tools" / "dev_commands").glob("*.py"):
            found = re.search(r'^NAME\s*=\s*"([a-z]+)"', path.read_text(encoding="utf-8"), re.MULTILINE)
            if found:
                commands.add(found.group(1))
        self.assertIn("trace", commands)
        doc = (REPO / "CLI.md").read_text(encoding="utf-8")
        for name in commands:
            self.assertIn(f"dev.py {name}", doc, f"CLI.md 未收录 dev.py 子命令 {name}")


class PerfGateTest(unittest.TestCase):
    """D2-13 回归闸门：5% 边界、方向、缺文件退出码。"""

    @staticmethod
    def _module():
        spec = importlib.util.spec_from_file_location(
            "ci_perf_gate", REPO / "scripts" / "ci" / "check_perf_baseline.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_throughput_boundary(self) -> None:
        gate = self._module()
        baseline = {"total_tok_s": 60.0, "ttft_p50_ms": 150.0}
        ok = {"total_tok_s": "57.1", "ttft_p50_ms": "150.0"}
        self.assertEqual(gate.compare(baseline, ok), [])
        bad = {"total_tok_s": "56.9", "ttft_p50_ms": "150.0"}
        self.assertTrue(gate.compare(baseline, bad))

    def test_ttft_direction(self) -> None:
        gate = self._module()
        baseline = {"total_tok_s": 60.0, "ttft_p50_ms": 150.0}
        self.assertEqual(gate.compare(baseline, {"total_tok_s": "60.0", "ttft_p50_ms": "157.4"}), [])
        self.assertTrue(gate.compare(baseline, {"total_tok_s": "60.0", "ttft_p50_ms": "157.6"}))

    def test_missing_inputs_exit_three(self) -> None:
        gate = self._module()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.assertEqual(gate.main(["--baseline", str(root / "no.json"),
                                        "--csv", str(root / "no.csv")]), 3)


if __name__ == "__main__":
    unittest.main()
