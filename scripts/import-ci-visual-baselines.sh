#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
ARTIFACTS_ROOT="${1:-}"

if [[ -z "$ARTIFACTS_ROOT" || ! -d "$ARTIFACTS_ROOT" ]]; then
  cat >&2 <<USAGE
Usage: $0 ARTIFACTS_ROOT

Import actual.png files from a downloaded GitHub Actions visual artifact
(lingtai-desktop-ui-artifacts) into tests/visual/baselines/macos/light/.

Example:
  gh run download RUN_ID -n lingtai-desktop-ui-artifacts -D /tmp/ci-ui-artifacts
  $0 /tmp/ci-ui-artifacts
USAGE
  exit 1
fi

BASELINE_DIR="$ROOT/tests/visual/baselines/macos/light"
mkdir -p "$BASELINE_DIR"

import_snapshot() {
  local snapshot_id="$1"
  local actual="$ARTIFACTS_ROOT/visual/agent_detail_visual/${snapshot_id}/actual.png"
  local dest="$BASELINE_DIR/${snapshot_id}.png"
  if [[ ! -f "$actual" ]]; then
    printf 'skip: missing %s\n' "$actual" >&2
    return 0
  fi
  cp "$actual" "$dest"
  printf 'imported %s -> %s\n' "$actual" "$dest"
}

import_snapshot "conversation-normal"
import_snapshot "presets-normal"
