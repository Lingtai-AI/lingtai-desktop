#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
RUN_ID="${1:-}"

if [[ -z "$RUN_ID" ]]; then
  cat >&2 <<USAGE
Usage: $0 RUN_ID

Download visual artifacts from a GitHub Actions run and promote actual.png
files into tests/visual/baselines/macos/{light,dark}/.

Example:
  $0 32317524218
USAGE
  exit 1
fi

tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/lingtai-ci-visual.XXXXXX")"
trap 'rm -rf "$tmpdir"' EXIT

gh run download "$RUN_ID" -n lingtai-desktop-ui-artifacts -D "$tmpdir"
"$ROOT/scripts/import-ci-visual-baselines.sh" "$tmpdir"
