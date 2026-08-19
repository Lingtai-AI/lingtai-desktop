#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
HOOKS_SRC="$ROOT/scripts/git-hooks"
GIT_HOOKS="$(git -C "$ROOT" rev-parse --git-dir)/hooks"

if [[ ! -d "$HOOKS_SRC" ]]; then
  printf 'install-git-hooks: missing %s\n' "$HOOKS_SRC" >&2
  exit 1
fi

mkdir -p "$GIT_HOOKS"
for hook in "$HOOKS_SRC"/*; do
  [[ -f "$hook" ]] || continue
  name="$(basename "$hook")"
  install -m 755 "$hook" "$GIT_HOOKS/$name"
  printf 'installed %s\n' "$name"
done

printf 'Git hooks installed. pre-push runs scripts/ci/preflight.sh\n'
