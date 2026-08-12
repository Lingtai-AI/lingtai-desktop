#!/usr/bin/env python3
"""Run the full lib_ui target smoke under Qt's offscreen platform plugin."""
from __future__ import annotations

import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD_DIR = pathlib.Path(os.environ.get("BUILD_DIR", ROOT / "build"))
QT_ROOT = pathlib.Path(os.environ.get("QT_ROOT", pathlib.Path.home() / "Qt/6.11.1/macos"))
EXECUTABLE = BUILD_DIR / "lingtai_desktop_smoke"
MARKER = "LINGTAI_LIB_UI_FULL_TARGET_SMOKE_OK"

if not EXECUTABLE.is_file():
    print(f"smoke: executable is absent: {EXECUTABLE}", file=sys.stderr)
    raise SystemExit(1)
if not (QT_ROOT / "plugins").is_dir():
    print(f"smoke: Qt plugin directory is absent: {QT_ROOT / 'plugins'}", file=sys.stderr)
    raise SystemExit(1)

environment = os.environ.copy()
environment["QT_QPA_PLATFORM"] = "offscreen"
environment["QT_PLUGIN_PATH"] = str(QT_ROOT / "plugins")
try:
    result = subprocess.run(
        [str(EXECUTABLE)],
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=8,
        check=False,
    )
except subprocess.TimeoutExpired as error:
    output = (error.stdout or "") + (error.stderr or "") + "\nSMOKE_TIMEOUT_AFTER_8_SECONDS\n"
    print(output, end="")
    raise SystemExit(1)

print(result.stdout, end="")
if result.returncode != 0 or MARKER not in result.stdout:
    print(f"smoke: failed with exit {result.returncode}", file=sys.stderr)
    raise SystemExit(1)
