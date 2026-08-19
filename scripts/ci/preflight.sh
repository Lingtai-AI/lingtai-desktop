#!/usr/bin/env bash
set -euo pipefail

# Local mirror of .github/workflows/ui-tests.yml — run before pushing to main.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
ARTIFACTS_DIR="${ARTIFACTS_DIR:-$ROOT/artifacts}"
QT_ROOT="${QT_ROOT:-${QT_ROOT_DIR:-$HOME/Qt/6.11.1/macos}}"

if [[ "$(uname -s)" != "Darwin" ]]; then
  printf 'preflight: macOS only (CI uses macos-15)\n' >&2
  exit 1
fi

if [[ -n "${UPDATE_UI_BASELINES:-}" ]]; then
  printf 'preflight: unset UPDATE_UI_BASELINES before preflight\n' >&2
  exit 1
fi

export QT_ROOT
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-cocoa}"
export ARTIFACTS_DIR

step() {
  printf '\n==> %s\n' "$1"
}

step "Install UI test fonts"
"$ROOT/scripts/ci/install-ui-test-fonts.sh"

if [[ "${PREFLIGHT_SKIP_BOOTSTRAP:-}" != "1" ]]; then
  step "Bootstrap dependencies"
  "$ROOT/scripts/bootstrap-deps.sh"
else
  step "Bootstrap dependencies (skipped: PREFLIGHT_SKIP_BOOTSTRAP=1)"
fi

step "Configure"
"$ROOT/scripts/configure.sh"

step "Build"
cmake --build "$BUILD_DIR" --parallel "${BUILD_JOBS:-8}"

step "Repository contract"
python3 -m unittest tests.test_repository_contract

step "UI tests (unit + ui + visual)"
"$ROOT/scripts/ci/run-ui-macos.sh"

printf '\npreflight: all checks passed\n'
