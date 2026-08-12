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
  lingtai_agent_manifest_discovery_test lingtai_agent_roster_presence_test \
  lingtai_agent_identity_status_test lingtai_direct_conversation_route_test \
  lingtai_workspace_selection_test lingtai_native_shell_test \
  --parallel "${BUILD_JOBS:-8}"
