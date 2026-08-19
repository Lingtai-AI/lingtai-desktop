#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  printf 'update-ui-baselines: configure build first (scripts/configure.sh)\n' >&2
  exit 1
fi

if [[ "$(uname -s)" != "Darwin" ]]; then
  printf 'update-ui-baselines: macOS only (visual baselines are macOS-specific)\n' >&2
  exit 1
fi

export UPDATE_UI_BASELINES=1
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-cocoa}"

cd "$BUILD_DIR"
echo "Updating visual baselines under tests/visual/baselines/macos/"
ctest -L visual --output-on-failure
