#!/usr/bin/env python3
"""Compile and run the Qt-independent C++ workspace-selection contract."""
from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class WorkspaceSelectionTest(unittest.TestCase):
    def compile_and_run_contract(self, compiler: str) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            temporary_path = Path(temporary)
            executable = temporary_path / "workspace_selection_test"
            compile_result = subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    "-I",
                    str(ROOT / "src"),
                    str(ROOT / "tests" / "workspace_selection_test.cpp"),
                    str(ROOT / "src" / "workspace_selection.cpp"),
                    str(ROOT / "src" / "project_attachment.cpp"),
                    "-o",
                    str(executable),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                compile_result.returncode,
                0,
                compile_result.stdout + compile_result.stderr,
            )
            run_result = subprocess.run(
                [str(executable), str(temporary_path / "fixture")],
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                run_result.returncode,
                0,
                run_result.stdout + run_result.stderr,
            )
            self.assertIn("WORKSPACE_SELECTION_CONTRACT_OK", run_result.stdout)

    def test_cpp_workspace_selection_contract(self) -> None:
        self.compile_and_run_contract(os.environ.get("CXX", "c++"))

    def test_gcc_14_portability(self) -> None:
        compiler = Path("/usr/local/bin/g++-14")
        if not compiler.is_file():
            self.skipTest(f"exact GCC portability compiler absent: {compiler}")
        self.compile_and_run_contract(str(compiler))


if __name__ == "__main__":
    unittest.main()
