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
            "src/native_shell.cpp",
            "src/native_shell.h",
            "src/agent_manifest_discovery.cpp",
            "src/agent_manifest_discovery.h",
            "src/agent_manifest_discovery_test_seam.h",
            "src/agent_roster_presence.h",
            "src/agent_roster_presence_test_seam.h",
            "src/compatibility_probe.cpp",
            "src/compatibility_probe.h",
            "src/project_attachment.cpp",
            "src/project_attachment.h",
            "src/workspace_selection.cpp",
            "src/workspace_selection.h",
            "tests/agent_manifest_discovery_test.cpp",
            "tests/agent_roster_presence_test.cpp",
            "tests/compatibility_probe_test.cpp",
            "tests/native_shell_test.cpp",
            "tests/project_attachment_test.cpp",
            "tests/test_native_shell.py",
            "tests/workspace_selection_test.cpp",
            "tests/test_project_attachment.py",
            "tests/test_workspace_selection.py",
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
        commands: dict[tuple[str, str], list[list[str]]] = {}
        for match in re.finditer(
            r"(?m)^[ \t]*(add_library|target_link_libraries)"
            r"\s*\(\s*([^\s()]+)([^)]*)\)",
            cmake,
        ):
            commands.setdefault((match[1], match[2]), []).append(
                match[3].split()
            )

        def arguments(command: str, target: str) -> set[str]:
            calls = commands.get((command, target), [])
            self.assertEqual(
                len(calls), 1, f"expected one {command}({target} ...) call"
            )
            return set(calls[0]) if calls else set()

        def links(target: str) -> dict[str, set[str]]:
            calls = commands.get(("target_link_libraries", target), [])
            self.assertTrue(calls, f"missing target_link_libraries({target} ...)")
            result = {scope: set() for scope in ("PUBLIC", "PRIVATE", "INTERFACE")}
            for call in calls:
                scope = None
                for argument in call:
                    if argument in result:
                        scope = argument
                    elif scope is None:
                        self.fail(
                            f"{target} dependency {argument!r} has no link visibility"
                        )
                    else:
                        result[scope].add(argument)
            return result

        def scopes(sections: dict[str, set[str]], dependency: str) -> set[str]:
            return {scope for scope, items in sections.items() if dependency in items}

        self.assertTrue(
            {"STATIC", "src/project_attachment.cpp"}
            <= arguments("add_library", "lingtai_desktop_core")
        )
        self.assertEqual(cmake.count("src/project_attachment.cpp"), 1)
        self.assertIn(
            "src/workspace_selection.cpp",
            arguments("add_library", "lingtai_desktop_core"),
        )
        self.assertEqual(cmake.count("src/workspace_selection.cpp"), 1)
        self.assertTrue(
            {"STATIC", "src/native_shell.cpp"}
            <= arguments("add_library", "lingtai_desktop_native_shell")
        )
        self.assertEqual(
            cmake.count("src/native_shell.cpp"),
            2,
            "production library plus focused sanitizer target",
        )
        self.assertEqual(
            links("lingtai_desktop_native_shell"),
            {
                "PUBLIC": {"lingtai_desktop_core"},
                "PRIVATE": {"desktop-app::lib_ui"},
                "INTERFACE": set(),
            },
        )
        self.assertTrue(
            {"STATIC", "src/compatibility_probe.cpp"}
            <= arguments("add_library", "lingtai_desktop_compatibility")
        )
        self.assertEqual(cmake.count("src/compatibility_probe.cpp"), 1)
        compatibility = links("lingtai_desktop_compatibility")
        self.assertEqual(scopes(compatibility, "lingtai_desktop_core"), {"PUBLIC"})
        self.assertEqual(scopes(compatibility, "Qt6::Core"), {"PRIVATE"})
        self.assertTrue(
            {"STATIC", "src/agent_manifest_discovery.cpp"}
            <= arguments("add_library", "lingtai_desktop_agent_discovery")
        )
        self.assertEqual(cmake.count("src/agent_manifest_discovery.cpp"), 1)
        discovery = links("lingtai_desktop_agent_discovery")
        self.assertEqual(
            discovery,
            {
                "PUBLIC": {"lingtai_desktop_core"},
                "PRIVATE": {"Qt6::Core"},
                "INTERFACE": set(),
            },
        )
        self.assertEqual(
            links("lingtai_agent_manifest_discovery_test"),
            {
                "PUBLIC": set(),
                "PRIVATE": {"lingtai_desktop_agent_discovery"},
                "INTERFACE": set(),
            },
        )
        self.assertRegex(
            cmake,
            r"add_test\(NAME\s+agent_manifest_discovery\s+"
            r"COMMAND\s+lingtai_agent_manifest_discovery_test\b",
        )
        self.assertEqual(
            links("lingtai_agent_roster_presence_test"),
            {
                "PUBLIC": set(),
                "PRIVATE": {"lingtai_desktop_agent_discovery"},
                "INTERFACE": set(),
            },
        )
        self.assertRegex(
            cmake,
            r"add_test\(NAME\s+agent_roster_presence\s+"
            r"COMMAND\s+lingtai_agent_roster_presence_test\b",
        )
        self.assertEqual(
            links("lingtai_workspace_selection_test"),
            {
                "PUBLIC": set(),
                "PRIVATE": {"lingtai_desktop_core"},
                "INTERFACE": set(),
            },
        )
        self.assertRegex(
            cmake,
            r"add_test\(NAME\s+workspace_selection\s+"
            r"COMMAND\s+lingtai_workspace_selection_test\b",
        )
        for header in (
            "src/agent_manifest_discovery.h",
            "src/agent_manifest_discovery_test_seam.h",
            "src/agent_roster_presence.h",
            "src/agent_roster_presence_test_seam.h",
            "src/native_shell.h",
            "src/workspace_selection.h",
        ):
            text = (ROOT / header).read_text()
            self.assertNotRegex(text, r"#\s*include\s*[<\"]Qt")
            self.assertNotIn("QT_CORE_LIB", text)
        self.assertEqual(
            links("lingtai_compatibility_probe_test"),
            {
                "PUBLIC": set(),
                "PRIVATE": {"lingtai_desktop_compatibility"},
                "INTERFACE": set(),
            },
        )
        self.assertEqual(
            links("lingtai_desktop_smoke"),
            {
                "PUBLIC": set(),
                "PRIVATE": {
                    "lingtai_desktop_native_shell",
                    "desktop-app::lib_ui",
                },
                "INTERFACE": set(),
            },
        )
        self.assertEqual(
            links("lingtai_native_shell_test"),
            {
                "PUBLIC": set(),
                "PRIVATE": {
                    "lingtai_desktop_native_shell",
                    "desktop-app::lib_ui",
                },
                "INTERFACE": set(),
            },
        )
        self.assertRegex(
            cmake,
            r"add_test\(NAME\s+native_shell_behavior\s+"
            r"COMMAND\s+lingtai_native_shell_test\b",
        )
        shell_source = (ROOT / "src" / "native_shell.cpp").read_text()
        for excluded in (
            "QFileDialog",
            "attach_project(",
            "probe_compatibility(",
            "discover_agent_manifests(",
            "project_agent_roster(",
        ):
            self.assertNotIn(excluded, shell_source)


if __name__ == "__main__":
    unittest.main()
