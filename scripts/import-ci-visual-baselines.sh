#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
ARTIFACTS_ROOT="${1:-}"

if [[ -z "$ARTIFACTS_ROOT" || ! -d "$ARTIFACTS_ROOT" ]]; then
  cat >&2 <<USAGE
Usage: $0 ARTIFACTS_ROOT

Import actual.png files from a downloaded GitHub Actions visual artifact
(lingtai-desktop-ui-artifacts) into tests/visual/baselines/macos/{light,dark}/.

Artifact folders use native_shell_visual/<surface>-<theme>-normal/ (current)
or native_shell_visual/<surface>-normal/ (legacy, treated as light).

Example:
  gh run download RUN_ID -n lingtai-desktop-ui-artifacts -D /tmp/ci-ui-artifacts
  $0 /tmp/ci-ui-artifacts
USAGE
  exit 1
fi

BASELINE_ROOT="$ROOT/tests/visual/baselines/macos"
mkdir -p "$BASELINE_ROOT/light" "$BASELINE_ROOT/dark"

import_native_shell() {
  local surface="$1"
  local theme="$2"
  local artifact_name="${surface}-${theme}-normal"
  local actual="$ARTIFACTS_ROOT/visual/native_shell_visual/${artifact_name}/actual.png"
  local dest="$BASELINE_ROOT/${theme}/${surface}-normal.png"

  if [[ ! -f "$actual" && "$theme" == "light" ]]; then
    actual="$ARTIFACTS_ROOT/visual/native_shell_visual/${surface}-normal/actual.png"
  fi
  if [[ ! -f "$actual" ]]; then
    printf 'skip: missing %s\n' "$actual" >&2
    return 0
  fi
  cp "$actual" "$dest"
  printf 'imported %s -> %s\n' "$actual" "$dest"
}

for surface in startup-idle setup-preset setup-agents setup-review \
    conversation presets kanban empty-conversation; do
  for theme in light dark; do
    import_native_shell "$surface" "$theme"
  done
done
