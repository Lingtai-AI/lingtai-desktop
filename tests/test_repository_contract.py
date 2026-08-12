#!/usr/bin/env python3
"""Focused repository-foundation contract for LingTai Desktop."""
from __future__ import annotations

import json
import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / "cmake" / "desktop-app-toolkit-lock.json"
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
    def test_locked_toolkit_foundation_is_complete_and_portable(self) -> None:
        data = json.loads(LOCK.read_text())
        self.assertEqual(data["qt"]["version"], "6.11.1")
        self.assertEqual(
            data["evidence"]["tdesktop_commit"],
            "8e18cb71103d83d7d98994ff27f0a2bca55c489c",
        )
        actual = {item["name"]: item["commit"] for item in data["sources"]}
        for name, commit in REQUIRED_TOOLKIT.items():
            self.assertEqual(actual[name], commit)
        required_files = (
            "LICENSE",
            "README.md",
            "AGENTS.md",
            "ANATOMY.md",
            "CMakeLists.txt",
            "scripts/bootstrap-deps.sh",
            "scripts/configure.sh",
            "scripts/build.sh",
            "scripts/smoke.py",
            "src/main.cpp",
            "src/crl_integration.cpp",
            "src/compatibility_probe.cpp",
            "src/compatibility_probe.h",
            "src/project_attachment.cpp",
            "src/project_attachment.h",
            "tests/compatibility_probe_test.cpp",
            "tests/project_attachment_test.cpp",
            "tests/test_project_attachment.py",
        )
        for relative in required_files:
            self.assertTrue((ROOT / relative).is_file(), relative)
        for script in sorted((ROOT / "scripts").glob("*.sh")):
            subprocess.run(["bash", "-n", str(script)], check=True)
        tracked = subprocess.check_output(
            ["git", "-C", ROOT, "ls-files"], text=True
        ).splitlines()
        self.assertTrue(set(required_files).issubset(tracked))
        forbidden = (".deps/", "build/", "Qt/", "/tmp/")
        self.assertFalse([path for path in tracked if path.startswith(forbidden)])
        validation_root_marker = "/tmp/" + "lingtai-lib-ui-validation"
        for path in tracked:
            if (ROOT / path).is_file():
                self.assertNotIn(
                    validation_root_marker, (ROOT / path).read_text(errors="ignore")
                )

        cmake = (ROOT / "CMakeLists.txt").read_text()
        commands = {
            (match[1], match[2]): set(match[3].split())
            for match in re.finditer(
                r"(add_library|target_link_libraries)\s*\(\s*(\S+)([^)]*)\)",
                cmake,
            )
        }
        self.assertTrue(
            {"STATIC", "src/project_attachment.cpp"}
            <= commands[("add_library", "lingtai_desktop_core")]
        )
        self.assertEqual(cmake.count("src/project_attachment.cpp"), 1)
        self.assertTrue(
            {"STATIC", "src/compatibility_probe.cpp"}
            <= commands[("add_library", "lingtai_desktop_compatibility")]
        )
        self.assertEqual(cmake.count("src/compatibility_probe.cpp"), 1)
        self.assertTrue(
            {"lingtai_desktop_core", "Qt6::Core"}
            <= commands[("target_link_libraries", "lingtai_desktop_compatibility")]
        )
        self.assertTrue(
            {"lingtai_desktop_compatibility", "desktop-app::lib_ui"}
            <= commands[("target_link_libraries", "lingtai_desktop_smoke")]
        )


if __name__ == "__main__":
    unittest.main()
