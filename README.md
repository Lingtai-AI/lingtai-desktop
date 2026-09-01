# LingTai Desktop

**The native macOS app for working with your LingTai agent network.**

Open or create a LingTai project, see its agents, and work with them from one
place. LingTai Desktop uses the same on-disk project model as the TUI and CLI,
so there is no duplicate configuration or second source of truth.

## Get started

LingTai Desktop requires macOS 13 or newer.

Install the current stable release:

```bash
curl -fsSL https://lingtai.ai/install.sh | bash
```

Then launch it:

```bash
lingtai-desktop
```

The installer does not change your shell's `PATH`. If the command is not found,
use the installed launcher's full path:

```bash
~/.local/bin/lingtai-desktop
```

### First run

Choose a directory when the app opens:

- If it already contains a `.lingtai` project, Desktop opens it.
- If it does not, Desktop guides you through creating the first orchestrator,
  opens the new project, and starts its Agent.

## Everyday commands

```bash
lingtai-desktop          # Launch the app
lingtai-desktop version  # Show the installed version and artifact digests
lingtai-desktop doctor   # Verify the managed installation and support state
lingtai-desktop update   # Update to the current stable release
```

## What you can do

- Open the same LingTai projects used by the TUI and CLI, or create a new one.
- See agents and their status, read direct conversations, and send messages and
  local attachments.
- Inspect agent details, manage agent lifecycle and setup, and review project
  work in the Kanban view.

## For contributors

### Requirements

- macOS 13.0 or newer
- CMake 3.25+, Ninja, Git, curl, Python 3, and Xcode command-line tools
- Qt 6.11.1 macOS universal distribution
- A managed LingTai kernel runtime for Agent launch

Set `QT_ROOT` to your Qt 6.11.1 prefix (default:
`$HOME/Qt/6.11.1/macos`).

### Build and run

```bash
export QT_ROOT="$HOME/Qt/6.11.1/macos"
./scripts/bootstrap-deps.sh
./scripts/configure.sh
./scripts/build.sh
open ./build/LingTai.app
# ./build/LingTai.app/Contents/MacOS/LingTai --smoke
```

Run the repository preflight for a CI-parity check:

```bash
./scripts/ci/preflight.sh
```

### Project map and contracts

- [AGENTS.md](AGENTS.md) — contributor validation checklist
- [ANATOMY.md](ANATOMY.md) — repository map and build graph
- [`cmake/desktop-app-toolkit-lock.json`](cmake/desktop-app-toolkit-lock.json) —
  exact dependency provenance and pins
- [Managed delivery](docs/managed-delivery.md) — archive, installation,
  update, support-bootstrap, and optional DMG engineering details

LingTai Desktop is a native Qt app built on a pinned
`desktop-app::lib_ui` toolkit slice. It is **not** a Telegram Desktop fork: it
contains no Telegram protocol, account, chat, or product-screen code. Owned
product code lives in `src/`; toolkit sources are fetched into ignored `.deps/`
at build time.

## License

Copyright 2026 LingTai contributors.

LingTai Desktop is licensed under GNU GPL version 3 with the OpenSSL exception
in [LICENSE](LICENSE), the same licensing model as
[Telegram Desktop](https://github.com/telegramdesktop/tdesktop/tree/f0ee75edef45fc1d6f3828e56a5d7600643136a2).
The 14-line exception is adapted from
[Telegram Desktop's LICENSE](https://github.com/telegramdesktop/tdesktop/blob/f0ee75edef45fc1d6f3828e56a5d7600643136a2/LICENSE);
only its project-identifying first line was changed. This license attribution
does not change the relationship between the projects: LingTai Desktop is not a
Telegram Desktop fork.
