#!/usr/bin/env python3
"""Static safety contract for Desktop-owned project staging rollback."""

from __future__ import annotations

import unittest
from pathlib import Path


SOURCE = Path(__file__).resolve().parents[1] / "src" / "project_creation.cpp"


class ProjectCreationSourceContractTest(unittest.TestCase):
    def test_rollback_is_descriptor_relative_not_path_recursive(self) -> None:
        implementation = SOURCE.read_text()
        self.assertNotIn("remove_all", implementation)
        for primitive in ("::openat", "::fstatat", "AT_SYMLINK_NOFOLLOW", "::unlinkat"):
            self.assertIn(primitive, implementation)


if __name__ == "__main__":
    unittest.main()
