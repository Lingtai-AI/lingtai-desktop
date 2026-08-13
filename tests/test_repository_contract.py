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
            "src/posix_descriptor_primitives.cpp",
            "src/posix_descriptor_primitives.h",
            "src/agent_manifest_discovery.cpp",
            "src/agent_manifest_discovery.h",
            "src/agent_manifest_discovery_test_seam.h",
            "src/agent_roster_presence.h",
            "src/agent_roster_presence_test_seam.h",
            "src/agent_identity_status.h",
            "src/agent_identity_status_test_seam.h",
            "src/direct_conversation_route.cpp",
            "src/direct_conversation_route.h",
            "src/direct_conversation_history.cpp",
            "src/direct_conversation_history.h",
            "src/direct_mail_publisher.cpp",
            "src/direct_mail_publisher.h",
            "src/compatibility_probe.cpp",
            "src/compatibility_probe.h",
            "src/project_attachment.cpp",
            "src/project_attachment.h",
            "src/workspace_selection.cpp",
            "src/workspace_selection.h",
            "tests/posix_descriptor_primitives_test.cpp",
            "tests/agent_manifest_discovery_test.cpp",
            "tests/agent_roster_presence_test.cpp",
            "tests/agent_identity_status_test.cpp",
            "tests/direct_conversation_route_test.cpp",
            "tests/direct_conversation_history_test.cpp",
            "tests/direct_mail_publisher_test.cpp",
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
        # Every registered test must be reachable from the documented build
        # path, or configure/build/ctest leaves it unbuilt and "Not Run".
        build_script = (ROOT / "scripts" / "build.sh").read_text()
        agents = (ROOT / "AGENTS.md").read_text()
        cmake_text = (ROOT / "CMakeLists.txt").read_text()
        documented = re.findall(r"ctest[^\n]*-R\s+'([^']+)'", agents)
        for name, target in re.findall(
            r"add_test\(NAME\s+(\S+)\s+COMMAND\s+(\S+)", cmake_text
        ):
            if not target.startswith("lingtai_"):
                continue
            self.assertIn(target, build_script, f"{target} is never built")
            self.assertTrue(
                any(re.search(pattern, name) for pattern in documented),
                f"{name} is in no documented validate step",
            )
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
                "PRIVATE": {
                    "desktop-app::lib_ui",
                    "lingtai_desktop_compatibility",
                    "lingtai_desktop_agent_discovery",
                    "lingtai_desktop_conversation",
                    "lingtai_desktop_mail_publisher",
                },
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
        self.assertEqual(
            cmake.count("src/agent_manifest_discovery.cpp"), 2,
            "production library plus focused native-shell sanitizer target",
        )
        discovery = links("lingtai_desktop_agent_discovery")
        self.assertEqual(
            discovery,
            {
                "PUBLIC": {"lingtai_desktop_core"},
                "PRIVATE": {"Qt6::Core", "lingtai_desktop_posix_primitives"},
                "INTERFACE": set(),
            },
        )
        # The shared descriptor seam is internal: one library, no link edge of
        # its own, and therefore no Qt, no project model, and no reader.
        self.assertTrue(
            {"STATIC", "src/posix_descriptor_primitives.cpp"}
            <= arguments("add_library", "lingtai_desktop_posix_primitives")
        )
        self.assertEqual(
            cmake.count("src/posix_descriptor_primitives.cpp"),
            3,
            "internal library plus the two focused sanitizer targets",
        )
        self.assertNotRegex(
            cmake,
            r"target_link_libraries\(\s*lingtai_desktop_posix_primitives",
            "the internal descriptor seam must depend on nothing",
        )
        self.assertEqual(
            links("lingtai_posix_descriptor_primitives_test"),
            {
                "PUBLIC": set(),
                "PRIVATE": {"lingtai_desktop_posix_primitives"},
                "INTERFACE": set(),
            },
        )
        self.assertRegex(
            cmake,
            r"add_test\(NAME\s+posix_descriptor_primitives\s+"
            r"COMMAND\s+lingtai_posix_descriptor_primitives_test\b",
        )
        self.assertRegex(
            cmake,
            r"add_executable\(lingtai_posix_descriptor_primitives_sanitized_test"
            r"\s+EXCLUDE_FROM_ALL[\s\S]*?src/posix_descriptor_primitives\.cpp"
            r"[\s\S]*?\)",
        )
        # Discovery consumes the shared primitives rather than re-deriving
        # them; the mechanics must exist in exactly one place.
        discovery_source = (
            ROOT / "src" / "agent_manifest_discovery.cpp"
        ).read_text()
        self.assertIn(
            '#include "posix_descriptor_primitives.h"', discovery_source
        )
        for duplicated in (
            r"class\s+FileDescriptor\b",
            r"class\s+DirectoryStream\b",
            r"struct\s+FileIdentity\b",
            r"int\s+read_flags\s*\(",
            r"bool\s+same_file\s*\(",
            r"double\s+modified_at_seconds\s*\(",
            r"bool\s+safe_leaf\s*\(",
        ):
            self.assertNotRegex(discovery_source, duplicated, duplicated)
        for shared in ("read_flags(", "same_file(", "safe_leaf("):
            self.assertNotIn(
                " " + shared,
                discovery_source,
                f"{shared} must be reached through the qualified seam",
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
            links("lingtai_agent_identity_status_test"),
            {
                "PUBLIC": set(),
                "PRIVATE": {"lingtai_desktop_agent_discovery"},
                "INTERFACE": set(),
            },
        )
        self.assertRegex(
            cmake,
            r"add_test\(NAME\s+agent_identity_status\s+"
            r"COMMAND\s+lingtai_agent_identity_status_test\b",
        )
        self.assertTrue(
            {"STATIC", "src/direct_conversation_route.cpp"}
            <= arguments("add_library", "lingtai_desktop_direct_route")
        )
        self.assertEqual(
            cmake.count("src/direct_conversation_route.cpp"),
            3,
            "production library, route sanitizer, and native-shell sanitizer",
        )
        self.assertEqual(
            links("lingtai_desktop_direct_route"),
            {
                "PUBLIC": {"lingtai_desktop_core"},
                "PRIVATE": set(),
                "INTERFACE": set(),
            },
            "pure route resolution links neither Qt nor Agent discovery",
        )
        self.assertEqual(
            links("lingtai_direct_conversation_route_test"),
            {
                "PUBLIC": set(),
                "PRIVATE": {"lingtai_desktop_direct_route"},
                "INTERFACE": set(),
            },
        )
        self.assertRegex(
            cmake,
            r"add_test\(NAME\s+direct_conversation_route\s+"
            r"COMMAND\s+lingtai_direct_conversation_route_test\b",
        )
        # Route resolution consumes one already-produced C3 snapshot. It must
        # never rediscover manifests or touch the filesystem at all; only the
        # already-attached canonical root getter may be observed.
        route_source = (ROOT / "src" / "direct_conversation_route.cpp").read_text()
        for forbidden in (
            r"\bopen\s*\(",
            r"\bopenat\b",
            r"\bfopen\b",
            r"\bl?stat\s*\(",
            r"\bfstat\b",
            r"\bopendir\b",
            r"\breaddir\b",
            r"\b[io]f?stream\b",
            r"\bdirectory_iterator\b",
            r"\bcanonical\b",
            r"\bexists\s*\(",
            r"\.resolve\s*\(",
            r"\bdiscover_agent_manifests\b",
            r"\bproject_agent_roster\b",
            r"\bproject_agent_identity_status\b",
            r"\.agent\.json",
            r"#\s*include\s*[<\"](unistd|fcntl|dirent|fstream|sys/|Qt)",
        ):
            self.assertNotRegex(route_source, forbidden, forbidden)
        self.assertEqual(route_source.count("attachment.root()"), 1)
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
            "src/posix_descriptor_primitives.h",
            "src/agent_manifest_discovery.h",
            "src/agent_manifest_discovery_test_seam.h",
            "src/agent_roster_presence.h",
            "src/agent_roster_presence_test_seam.h",
            "src/agent_identity_status.h",
            "src/agent_identity_status_test_seam.h",
            "src/direct_conversation_route.h",
            "src/direct_conversation_history.h",
            "src/direct_mail_publisher.h",
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
        self.assertEqual(
            links("lingtai_native_shell_sanitized_test"),
            {
                "PUBLIC": set(),
                "PRIVATE": {
                    "lingtai_desktop_compatibility",
                    "desktop-app::lib_ui",
                },
                "INTERFACE": set(),
            },
        )
        shell_source = (ROOT / "src" / "native_shell.cpp").read_text()
        for excluded in (
            "QFileDialog",
            "discover_agent_manifests(",
            "project_agent_roster(",
        ):
            self.assertNotIn(excluded, shell_source)
        self.assertIn("attach_project(", shell_source)
        self.assertIn("probe_compatibility(", shell_source)
        self.assertEqual(shell_source.count("project_agent_identity_status("), 1)
        self.assertRegex(
            cmake,
            r"add_executable\(lingtai_native_shell_sanitized_test\s+"
            r"EXCLUDE_FROM_ALL[\s\S]*?src/agent_manifest_discovery\.cpp"
            r"[\s\S]*?\)",
        )
        main_source = (ROOT / "src" / "main.cpp").read_text()
        self.assertEqual(main_source.count("QFileDialog::getExistingDirectory"), 1)
        self.assertIn("QFileDialog::ShowDirsOnly", main_source)
        self.assertIn("selected.isEmpty()", main_source)
        self.assertIn(".lingtai-tui/install.json", main_source)
        self.assertNotIn("attach_project(", main_source)
        self.assertNotIn("probe_compatibility(", main_source)


if __name__ == "__main__":
    unittest.main()
