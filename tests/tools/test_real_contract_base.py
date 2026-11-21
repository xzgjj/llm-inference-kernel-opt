"""real_service_contract：normalize_base 与容错响应解析的 hermetic 单测（不起真服务）。"""
from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
E2E_MODULE = REPO_ROOT / "tests" / "e2e" / "test_real_service_contract.py"

_spec = importlib.util.spec_from_file_location("real_contract", E2E_MODULE)
contract = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(contract)


class NormalizeBaseTest(unittest.TestCase):
    def test_http_host_port(self) -> None:
        self.assertEqual(contract.normalize_base("http://h:8080"), ("h", 8080))

    def test_http_default_port_80(self) -> None:
        self.assertEqual(contract.normalize_base("http://h"), ("h", 80))
        self.assertEqual(contract.normalize_base("http://h/"), ("h", 80))

    def test_https_default_port_443(self) -> None:
        self.assertEqual(contract.normalize_base("https://h"), ("h", 443))

    def test_rejects_path(self) -> None:
        with self.assertRaises(ValueError):
            contract.normalize_base("http://h:8080/v1")

    def test_rejects_bad_scheme(self) -> None:
        with self.assertRaises(ValueError):
            contract.normalize_base("ftp://h:8080")

    def test_rejects_missing_host(self) -> None:
        with self.assertRaises(ValueError):
            contract.normalize_base("http://")


class BodyParseTest(unittest.TestCase):
    def test_non_json_body_returns_empty(self) -> None:
        self.assertEqual(contract.parse_json_body(b"<html>boom</html>"), {})

    def test_json_array_is_not_object(self) -> None:
        self.assertEqual(contract.parse_json_body(b"[1, 2, 3]"), {})

    def test_error_object_missing_or_wrong_type(self) -> None:
        self.assertEqual(contract.error_object(b"not json"), {})
        self.assertEqual(contract.error_object(b'{"error": "oops"}'), {})

    def test_error_object_extracted(self) -> None:
        body = b'{"error": {"code": "INVALID_JSON", "message": "bad"}}'
        self.assertEqual(contract.error_object(body).get("code"), "INVALID_JSON")


if __name__ == "__main__":
    unittest.main()
