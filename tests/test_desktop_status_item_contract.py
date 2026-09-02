#!/usr/bin/env python3
"""Static guard for the bounded macOS Desktop status-item slice."""
from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PLIST = ROOT / "cmake" / "macos" / "Info.plist.in"
STATUS_HEADER = ROOT / "src" / "desktop_status_item.h"
STATUS_SOURCE = ROOT / "src" / "desktop_status_item.cpp"
SHELL_HOST_SOURCE = ROOT / "src" / "shell_host.cpp"


class DesktopStatusItemContractTest(unittest.TestCase):
    def test_desktop_remains_a_normal_dock_application(self) -> None:
        plist = PLIST.read_text()
        self.assertNotIn("LSUIElement", plist)
        self.assertNotIn("LSBackgroundOnly", plist)

        product_sources = "\n".join(
            path.read_text(errors="strict")
            for path in (ROOT / "src").rglob("*")
            if path.suffix in {".cpp", ".h", ".mm"}
        )
        self.assertNotIn("setQuitOnLastWindowClosed(false)", product_sources)

    def test_status_item_adds_no_resident_runtime_mechanism(self) -> None:
        status_item = STATUS_HEADER.read_text() + STATUS_SOURCE.read_text()
        for forbidden in (
            "QProcess",
            "QThread",
            "QTimer",
            "QNetwork",
            "std::thread",
            "std::jthread",
            "fork(",
            "popen(",
            "poll(",
            "showMessage(",
            "isSystemTrayAvailable",
            "AppKit",
            "Cocoa",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, status_item)

    def test_show_path_restores_raises_and_activates_an_owned_shell(self) -> None:
        shell_host = SHELL_HOST_SOURCE.read_text()
        self.assertIn("window.showNormal()", shell_host)
        self.assertIn("window.raise()", shell_host)
        self.assertIn("window.activateWindow()", shell_host)


if __name__ == "__main__":
    unittest.main()
