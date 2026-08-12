# LingTai Desktop

LingTai Desktop is a GPL-3.0-or-later native-desktop foundation. It adopts the
Telegram Desktop App Toolkit at a deliberately narrow boundary: the complete,
pinned `desktop-app::lib_ui` target is built from source and exercised through a
real `Ui::RpWidget` smoke executable.

This repository intentionally does **not** import Telegram product code. It has
no Telegram protocol, accounts, contacts, chats, messages, media, cache, or
product screens. The owned `src/` files are the initial LingTai integration
surface; the toolkit source and its required parent icon resources are fetched
into ignored `.deps/` directories.

## Requirements

The validated environment is macOS with CMake 3.25+, Ninja, Git, curl, Python
3, Clang/Xcode command-line tools, and the official universal Qt 6.11.1 macOS
distribution. No script installs system packages.

Set `QT_ROOT` to the Qt 6.11.1 macOS prefix. If it is omitted, the scripts use
`$HOME/Qt/6.11.1/macos`. `QT_ROOT` must contain `lib/cmake/Qt6` and the Qt
plugins directory.

## Build and smoke

```bash
export QT_ROOT="$HOME/Qt/6.11.1/macos"  # or your official Qt 6.11.1 prefix
./scripts/bootstrap-deps.sh
./scripts/configure.sh
./scripts/build.sh
./scripts/smoke.py
```

`bootstrap-deps.sh` fetches only lock-manifest sources into `.deps/`, verifies
existing checkouts are clean and at their exact pinned commits, builds the
ignored universal OpenSSL archives locally, and fetches/verifies the three
parent resource Git blobs. It never uses a package manager or system installer.
For a network-free reuse of independently obtained exact checkouts, pass generic
local roots such as `--local-toolkit-root /path/to/toolkit` and
`--local-third-party-root /path/to/third-party`; the script validates source
remotes, commits, and cleanliness before cloning those selected checkouts.

The build target is `lingtai_desktop_smoke`. The smoke runner sets Qt's offscreen
platform plugin and requires the executable's `LINGTAI_LIB_UI_FULL_TARGET_SMOKE_OK`
marker.

## Dependency boundary

The machine-readable exact lock is
[`cmake/desktop-app-toolkit-lock.json`](cmake/desktop-app-toolkit-lock.json).
It records Qt, every toolkit and third-party source used by the minimal build,
the matching tdesktop comparison commit, checksum-pinned helper files, and the
three fetched resource blobs. See [ANATOMY.md](ANATOMY.md) for the source and
build topology.

## License

Copyright 2026 LingTai contributors.

LingTai Desktop is licensed under the GNU General Public License, version 3 or
later. See [LICENSE](LICENSE).
