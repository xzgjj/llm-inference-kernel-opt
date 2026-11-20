"""阶段 4 扫描器自测：依赖清零、规模口径、范围与测试登记。"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts" / "ci"))

import check_deps  # noqa: E402
import scope_scan  # noqa: E402
import size_dep_check  # noqa: E402


class DependencyTest(unittest.TestCase):
    def test_repo_has_no_paper_dependency(self) -> None:
        ok, messages = check_deps.check()
        self.assertTrue(ok, messages)
        self.assertEqual(check_deps.linked(), {"httplib", "nlohmann_json", "spdlog", "doctest"})

    def test_removed_dependency_is_detected(self) -> None:
        text = (
            "FetchContent_Declare(benchmark URL x)\n"
            "FetchContent_MakeAvailable(benchmark)\n"
            "target_link_libraries(core PRIVATE benchmark)\n"
        )
        with mock.patch.object(check_deps, "read_all", return_value=text):
            ok, messages = check_deps.check()
        self.assertFalse(ok)
        self.assertTrue(any("benchmark" in message for message in messages))


class SizeTest(unittest.TestCase):
    def test_repo_scale_within_budget(self) -> None:
        lines = size_dep_check.production_lines()
        self.assertGreater(lines, 4500)
        self.assertLessEqual(lines, 25000)
        self.assertLessEqual(size_dep_check.module_count(), 10)

    def test_sloc_ignores_blank_and_comment(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "a.cpp"
            path.write_text("\n// c\n/* block */\nint x;\n", encoding="utf-8")
            self.assertEqual(size_dep_check.sloc(path), 1)


class ScopeTest(unittest.TestCase):
    def test_repo_scope_is_clean(self) -> None:
        self.assertEqual(scope_scan.scan_terms(), [])
        self.assertEqual(scope_scan.scan_tests(), [])
        self.assertEqual(scope_scan.scan_gates(), [])


if __name__ == "__main__":
    unittest.main()
