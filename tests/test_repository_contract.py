#!/usr/bin/env python3
"""Pinned toolkit provenance, product version, and artifact guard for Desktop.

This is deliberately the sole owner of the pinned-commit/lock-file check: no
other test asserts that `cmake/desktop-app-toolkit-lock.json` still names the
exact verified upstream commits. Its one CMake-source assertion owns the
canonical product version; it is not a general repository-shape suite.
"""
from __future__ import annotations

import json
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / "cmake" / "desktop-app-toolkit-lock.json"
CMAKE = ROOT / "CMakeLists.txt"
REQUIRED_TOOLKIT = {
    "lib_ui": "9e9e5e1fc5ba7e3f4d5407b484210db3b46aa53d",
    "lib_base": "82d182a275e197fd717fecc86193d9d91f4fc5b5",
    "lib_rpl": "c57cccffb01d85570decd7fccb88419c9a682e63",
    "lib_crl": "7a165302fed408c84b2d1c2513e35a21a141da44",
    "codegen": "51cc8c564555914f0cf2f9eba9e9ea9df339192a",
    "cmake_helpers": "f79a0e6acdae261270391253d24f47efb54e9a7d",
    "legal": "81c3a0ebf04dca9ffc49c9a06a922fea34b01892",
}


class RepositoryContractTest(unittest.TestCase):
    def test_canonical_product_version_is_v019(self) -> None:
        declarations = [
            line for line in CMAKE.read_text().splitlines()
            if line.startswith("project(lingtai_desktop VERSION ")
        ]
        self.assertEqual(declarations, [
            "project(lingtai_desktop VERSION 0.1.9 LANGUAGES C CXX OBJC OBJCXX)",
        ])

    def test_locked_toolkit_foundation_is_pinned(self) -> None:
        data = json.loads(LOCK.read_text())
        self.assertEqual(data["qt"]["version"], "6.11.1")
        self.assertEqual(
            data["evidence"]["tdesktop_commit"],
            "8e18cb71103d83d7d98994ff27f0a2bca55c489c",
        )
        actual = {item["name"]: item["commit"] for item in data["sources"]}
        for name, commit in REQUIRED_TOOLKIT.items():
            self.assertEqual(actual[name], commit)

    def test_no_build_artifacts_or_dependency_trees_are_tracked(self) -> None:
        tracked = subprocess.check_output(
            ["git", "-C", ROOT, "ls-files"], text=True
        ).splitlines()
        forbidden = (".deps/", "build/", "Qt/", "/tmp/")
        self.assertFalse([path for path in tracked if path.startswith(forbidden)])
        validation_root_marker = "/tmp/" + "lingtai-lib-ui-validation"
        for path in tracked:
            if (ROOT / path).is_file():
                self.assertNotIn(
                    validation_root_marker, (ROOT / path).read_text(errors="ignore")
                )


if __name__ == "__main__":
    unittest.main()
