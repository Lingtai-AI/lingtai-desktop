#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
ARTIFACTS_DIR="${ARTIFACTS_DIR:-$ROOT/artifacts}"

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  "$ROOT/scripts/configure.sh"
fi

cmake --build "$BUILD_DIR" --parallel "${BUILD_JOBS:-8}"

mkdir -p "$ARTIFACTS_DIR/visual" "$ARTIFACTS_DIR/logs"
export ARTIFACTS_DIR

if [[ "$(uname -s)" == "Darwin" ]]; then
  export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-cocoa}"
fi

cd "$ROOT"
echo "==> repository contract"
python3 -m unittest tests.test_repository_contract

cd "$BUILD_DIR"

echo "==> unit tests"
ctest -L unit --output-on-failure

echo "==> functional UI tests"
ctest -L ui --output-on-failure

echo "==> visual snapshot tests"
if ctest -N -L visual 2>/dev/null | grep -q 'Total Tests: 0'; then
  echo "(no visual tests registered yet — skipping)"
else
  if ! ctest -L visual --output-on-failure; then
    echo "Visual failures: inspect $ARTIFACTS_DIR/visual/ (baseline, actual, diff, metadata.json)" >&2
    exit 1
  fi
fi

echo "UI tests finished. Artifacts: $ARTIFACTS_DIR"
