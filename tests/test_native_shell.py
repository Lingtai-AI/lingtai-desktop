#!/usr/bin/env python3
"""Focused process-level behavior contract for the native Desktop shell."""
from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys
import unittest


def shell_environment() -> dict[str, str]:
    environment = os.environ.copy()
    environment["QT_QPA_PLATFORM"] = (
        "cocoa" if sys.platform == "darwin" else "offscreen"
    )
    return environment


class NativeShellProcessTest(unittest.TestCase):
    executable: pathlib.Path

    def test_explicit_smoke_reports_constructed_shell(self) -> None:
        result = subprocess.run(
            [str(self.executable), "--smoke"],
            env=shell_environment(),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=4,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        lines = result.stdout.splitlines()
        marker_lines = [line for line in lines if line.startswith(
            "LINGTAI_LIB_UI_FULL_TARGET_SMOKE_OK"
        )]
        ready_lines = [line for line in lines if line == (
            "LINGTAI_NATIVE_SHELL_READY"
        )]
        self.assertEqual(len(ready_lines), 1, result.stdout)
        self.assertEqual(len(marker_lines), 1, result.stdout)
        self.assertLess(
            lines.index(ready_lines[0]),
            lines.index(marker_lines[0]),
            result.stdout,
        )
        self.assertIn("class=Ui::RpWindow", marker_lines[0])
        self.assertIn("window=lingtai_desktop_window", marker_lines[0])
        self.assertIn("body=lingtai_desktop_body", marker_lines[0])
        self.assertIn("sidebar=lingtai_desktop_sidebar", marker_lines[0])
        self.assertIn("content=lingtai_desktop_content", marker_lines[0])
        self.assertIn("separator=lingtai_roster_separator", marker_lines[0])
        self.assertIn("roster=lingtai_agent_roster", marker_lines[0])
        self.assertIn("empty=visible", marker_lines[0])
        self.assertIn("offscreen=true", marker_lines[0])
        self.assertIn("shown=true", marker_lines[0])
        self.assertNotIn("malformed logging rule", result.stdout)
        self.assertNotIn("Open Sans", result.stdout)

    def test_normal_mode_remains_open(self) -> None:
        process = subprocess.Popen(
            [str(self.executable), "--offscreen"],
            env=shell_environment(),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            output, _ = process.communicate(timeout=0.75)
        except subprocess.TimeoutExpired:
            process.terminate()
            try:
                process.communicate(timeout=1)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate(timeout=1)
            return

        self.fail(
            "normal execution exited during the persistence probe "
            f"(exit={process.returncode}); output:\n{output}"
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=pathlib.Path, required=True)
    arguments, unittest_arguments = parser.parse_known_args()
    NativeShellProcessTest.executable = arguments.executable.resolve()
    unittest.main(argv=[__file__, *unittest_arguments])
