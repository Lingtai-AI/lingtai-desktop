#!/usr/bin/env bash
set -euo pipefail

# Install Open Sans system-wide on macOS CI so Qt resolves the lib_ui family
# even before bundled test fonts are loaded. Bundled fonts under tests/fonts/
# remain the primary deterministic source for visual baselines.
if [[ "$(uname -s)" != "Darwin" ]]; then
  exit 0
fi

if ls "$HOME"/Library/Fonts/OpenSans*.ttf /Library/Fonts/OpenSans*.ttf \
    2>/dev/null | grep -q .; then
  echo "Open Sans already installed"
  exit 0
fi

if ! command -v brew >/dev/null; then
  echo "install-ui-test-fonts: brew unavailable; relying on bundled test fonts" >&2
  exit 0
fi

export HOMEBREW_NO_AUTO_UPDATE=1
if ! brew install --cask font-open-sans; then
  echo "install-ui-test-fonts: brew install failed; relying on bundled test fonts" >&2
fi
