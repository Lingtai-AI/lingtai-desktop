#!/usr/bin/env python3
"""Static safety contract for Desktop-owned project staging rollback."""

from __future__ import annotations

import unittest
from pathlib import Path


SOURCE = Path(__file__).resolve().parents[1] / "src" / "project_creation.cpp"
CONTENT_SOURCE = (
    Path(__file__).resolve().parents[1] / "src" / "project_creation_resources.cpp"
)
CMAKE = Path(__file__).resolve().parents[1] / "CMakeLists.txt"


class ProjectCreationSourceContractTest(unittest.TestCase):
    def test_rollback_is_descriptor_relative_not_path_recursive(self) -> None:
        implementation = SOURCE.read_text()
        self.assertNotIn("remove_all", implementation)
        for primitive in ("::openat", "::fstatat", "AT_SYMLINK_NOFOLLOW", "::unlinkat"):
            self.assertIn(primitive, implementation)

    def test_creation_has_no_tui_or_runtime_readiness_adapter(self) -> None:
        self.assertTrue(
            CONTENT_SOURCE.is_file(),
            "Desktop-owned localized project-creation resources are missing",
        )
        implementation = SOURCE.read_text() + CONTENT_SOURCE.read_text()
        for forbidden in (
            "lingtai-tui",
            "project_create.go",
            "tui/internal",
            "QProcess",
            "runtime_python_available",
        ):
            self.assertNotIn(forbidden, implementation)
        self.assertIn("src/project_creation_resources.cpp", CMAKE.read_text())


if __name__ == "__main__":
    unittest.main()
