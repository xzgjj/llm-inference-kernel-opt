"""CI 工具自测：规则也是代码，也需要测试。

rule: CI-TST-001
"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts" / "ci"))

import check_diff  # noqa: E402
import check_docs  # noqa: E402
import check_tests  # noqa: E402
import ci_common  # noqa: E402


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


if __name__ == "__main__":
    unittest.main()
