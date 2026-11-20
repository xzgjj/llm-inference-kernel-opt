"""test — ctest 包装。

- 无 ctest / 无测试目标：提示并退出 0（优雅降级）
- 无 GPU 自动补 nogpu 标签
- 解析 junit xml 汇总通过/失败；失败打印首个断言差异与复现命令，退出 1
"""

from __future__ import annotations

import shutil
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from dev_commands import common

NAME = "test"
HELP = "ctest 包装：标签/过滤/junit 汇总；无测试目标退出 0，失败退出 1"


def _has_test_target(root: Path) -> bool:
    if not (root / "CMakeLists.txt").exists():
        return False
    return any(root.glob("build/**/CTestTestfile.cmake"))


def _gpu_present() -> bool:
    probe, _ = common.load_probe()
    if probe is None:
        return False
    try:
        report = probe.build_report()
    except Exception:
        return False
    return any(g.get("vram_total_bytes") for g in report.get("gpus", []))


def _parse_junit(xml_path: Path) -> dict:
    tree = ET.parse(xml_path)
    root_el = tree.getroot()
    suite = root_el.find(".//testsuite") or root_el
    tests = int(suite.get("tests", 0))
    failures = int(suite.get("failures", 0))
    errors = int(suite.get("errors", 0))
    skipped = int(suite.get("skipped", 0))
    first_failure: dict | None = None
    for case in suite.iter("testcase"):
        failure = case.find("failure") or case.find("error")
        if failure is not None:
            first_failure = {
                "name": case.get("name"),
                "classname": case.get("classname"),
                "message": failure.get("message") or (failure.text or "").strip(),
            }
            break
    return {
        "tests": tests,
        "failures": failures,
        "errors": errors,
        "skipped": skipped,
        "passed": tests - failures - errors - skipped,
        "first_failure": first_failure,
    }


def add_arguments(parser) -> None:
    parser.add_argument("-k", dest="pattern", default=None, help="只跑匹配 PATTERN 的用例")
    parser.add_argument("--label", choices=["fast", "slow", "nogpu"], default=None)
    parser.add_argument("--bench", action="store_true", help="运行基准用例（标签 bench）")


def run(args) -> int:
    root = common.repo_root()
    ctest = shutil.which("ctest")

    if not ctest or not _has_test_target(root):
        msg = "无 ctest/测试目标，跳过测试"
        print(msg)
        if getattr(args, "json", False):
            common.print_json({"skipped": True, "reason": msg})
        return common.EXIT_OK

    labels: list[str] = []
    if args.label:
        labels.append(args.label)
    if args.bench:
        labels.append("bench")
    if not _gpu_present():
        labels.append("nogpu")  # 无 GPU 自动加 nogpu
        print("未检测到 GPU，自动追加 nogpu 标签")

    test_dir = root / "build" / "dev"
    xml_path = root / "build" / "test-results" / "junit.xml"
    xml_path.parent.mkdir(parents=True, exist_ok=True)

    argv = [ctest, "--test-dir", str(test_dir), f"--output-junit={xml_path}"]
    if args.pattern:
        argv += ["-R", args.pattern]
    if labels:
        argv += ["-L", "|".join(labels)]

    rc, out, err = common.run_cmd(argv, timeout=600)

    if xml_path.exists():
        summary = _parse_junit(xml_path)
    else:
        summary = {"tests": 0, "failures": 0, "errors": 0, "skipped": 0,
                   "passed": 0, "first_failure": None}

    repro = " ".join(argv)
    if rc != 0 or summary["failures"] or summary["errors"]:
        print(f"测试失败: {summary['passed']} 通过 / {summary['failures']} 失败 / "
              f"{summary['errors']} 错误 / {summary['skipped']} 跳过", file=sys.stderr)
        if summary["first_failure"]:
            ff = summary["first_failure"]
            print(f"首个失败: {ff['name']}\n{ff['message']}", file=sys.stderr)
        print(f"复现命令: {repro}", file=sys.stderr)
        if getattr(args, "json", False):
            common.print_json({"failed": True, "summary": summary, "repro": repro})
        return common.EXIT_TEST_FAIL

    print(f"测试通过: {summary['passed']} 通过 / {summary['skipped']} 跳过")
    if getattr(args, "json", False):
        common.print_json({"passed": True, "summary": summary, "repro": repro})
    return common.EXIT_OK
