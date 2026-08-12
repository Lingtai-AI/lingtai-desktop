#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
[[ -f "$BUILD_DIR/CMakeCache.txt" ]] || {
  printf 'build: no configured build tree at %s; run scripts/configure.sh first\n' "$BUILD_DIR" >&2
  exit 1
}
cmake --build "$BUILD_DIR" --target \
  lingtai_desktop_smoke lingtai_compatibility_probe_test \
  --parallel "${BUILD_JOBS:-8}"
