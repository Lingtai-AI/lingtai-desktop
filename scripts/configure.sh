#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
QT_ROOT="${QT_ROOT:-$HOME/Qt/6.11.1/macos}"
ARCHITECTURES="${CMAKE_OSX_ARCHITECTURES:-arm64;x86_64}"
DEPLOYMENT_TARGET="${CMAKE_OSX_DEPLOYMENT_TARGET:-13.0}"

[[ -d "$QT_ROOT/lib/cmake/Qt6" ]] || {
  printf 'configure: Qt 6.11.1 prefix is absent or invalid: %s\n' "$QT_ROOT" >&2
  printf 'configure: set QT_ROOT to the official Qt 6.11.1 macOS prefix.\n' >&2
  exit 1
}
command -v cmake >/dev/null || { printf 'configure: cmake not found\n' >&2; exit 1; }
command -v ninja >/dev/null || { printf 'configure: ninja not found\n' >&2; exit 1; }

cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
  -DQT_ROOT="$QT_ROOT" \
  -DCMAKE_PREFIX_PATH="$QT_ROOT" \
  -DCMAKE_OSX_ARCHITECTURES="$ARCHITECTURES" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
  -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
