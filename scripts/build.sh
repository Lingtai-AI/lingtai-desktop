#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
[[ -f "$BUILD_DIR/CMakeCache.txt" ]] || {
  printf 'build: no configured build tree at %s; run scripts/configure.sh first\n' "$BUILD_DIR" >&2
  exit 1
}
cmake --build "$BUILD_DIR" --target \
  lingtai_desktop_smoke \
  lingtai_agent_projection_test lingtai_direct_conversation_route_test \
  lingtai_direct_conversation_history_test \
  lingtai_conversation_unread_test \
  lingtai_direct_mail_publisher_test \
  lingtai_posix_descriptor_primitives_test \
  lingtai_workspace_selection_test lingtai_project_attachment_test \
  lingtai_native_shell_test lingtai_composer_spellcheck_test \
  --parallel "${BUILD_JOBS:-8}"
