#!/usr/bin/env python3
"""Static guard for the bounded macOS Desktop status-item slice."""
from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PLIST = ROOT / "cmake" / "macos" / "Info.plist.in"
STATUS_HEADER = ROOT / "src" / "desktop_status_item.h"
STATUS_SOURCE = ROOT / "src" / "desktop_status_item.cpp"


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

    def test_status_item_uses_only_template_safe_qt_rendering(self) -> None:
        header = STATUS_HEADER.read_text()
        source = STATUS_SOURCE.read_text()
        self.assertIn("set_unread_count", header)
        self.assertIn("QPainter", source)
        self.assertIn("QImage::Format_Alpha8", source)
        self.assertIn("icon.setIsMask(true)", source)
        self.assertIn("StatusItemTemplate.png", source)
        self.assertIn("StatusItemTemplate@2x.png", source)
        for forbidden in (
            "QColor",
            "Qt::black",
            "Qt::white",
            "Qt::red",
            "QLocale",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, source)

        allowed_qt_headers = {
            "<QtCore/QObject>",
            "<QtCore/QRect>",
            "<QtCore/QRectF>",
            "<QtCore/QSize>",
            "<QtCore/QString>",
            "<QtGui/QFont>",
            "<QtGui/QFontMetrics>",
            "<QtGui/QIcon>",
            "<QtGui/QImage>",
            "<QtGui/QPainter>",
            "<QtGui/QPixmap>",
            "<QtWidgets/QMenu>",
            "<QtWidgets/QSystemTrayIcon>",
        }
        qt_headers = {
            line.removeprefix("#include ").strip()
            for line in (header + source).splitlines()
            if line.startswith("#include <Qt")
        }
        self.assertEqual(qt_headers, allowed_qt_headers)

if __name__ == "__main__":
    unittest.main()
