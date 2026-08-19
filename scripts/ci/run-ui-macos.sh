#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
ARTIFACTS_DIR="${ARTIFACTS_DIR:-$ROOT/artifacts}"

if [[ "$(uname -s)" != "Darwin" ]]; then
  printf 'run-ui-macos: expected macOS runner\n' >&2
  exit 1
fi

export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-cocoa}"
export ARTIFACTS_DIR

mkdir -p "$ARTIFACTS_DIR/visual" "$ARTIFACTS_DIR/logs"

cd "$BUILD_DIR"
ctest -L unit --output-on-failure
ctest -L ui --output-on-failure

if ctest -N -L visual 2>/dev/null | grep -q 'Total Tests: 0'; then
  echo "(no visual tests registered yet — skipping)"
else
  ctest -L visual --output-on-failure
fi
