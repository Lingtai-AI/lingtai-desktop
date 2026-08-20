# LingTai Desktop

**The native app for your LingTai agent network.**

LingTai Desktop is the macOS client for [LingTai](https://github.com/TZZheng/lingtai) — open a project, see your agents, talk to them, and manage the network from one place. It reads the same on-disk project model as the TUI and CLI: no duplicate config, no second source of truth.

## What you can do

- **Open a LingTai project** — attach to an existing network or create your first orchestrator through a guided setup wizard.
- **Browse agents** — roster with live status, selection, and quick navigation across the network.
- **Talk to agents** — direct conversation surface with history, composer, and slash commands.
- **Inspect an agent** — context usage, presets, capabilities, token stats, and network membership on a detail page.
- **Track work** — kanban view over agent activity and session windows.
- **Manage presets** — discover and apply saved orchestrator configurations without leaving the app.

Desktop delegates project creation and preset/spawn operations to `lingtai-tui` subprocesses. It does not hand-edit `.lingtai` trees or invent configuration the TUI would not write.

## Requirements

- macOS (validated on macOS 15 in CI)
- CMake 3.25+, Ninja, Git, curl, Python 3, Xcode command-line tools
- Qt 6.11.1 macOS universal distribution
- `lingtai-tui` on `PATH` for setup, presets, and spawn flows

Set `QT_ROOT` to your Qt 6.11.1 prefix (default: `$HOME/Qt/6.11.1/macos`).

## Build and run

```bash
export QT_ROOT="$HOME/Qt/6.11.1/macos"
./scripts/bootstrap-deps.sh
./scripts/configure.sh
./scripts/build.sh
open ./build/LingTai.app            # Dock/Finder icon; omit --smoke for the full shell
# ./build/LingTai.app/Contents/MacOS/LingTai --smoke
```

For a CI-parity check before pushing:

```bash
./scripts/ci/preflight.sh
```

## First project

If you have no LingTai project yet, use **Create project** in the app. Desktop runs `lingtai-tui presets` to list choices, then `lingtai-tui spawn <destination> --preset <name>` when you confirm. The new project opens through the normal attach path.

## Architecture

LingTai Desktop is a native Qt app built on a pinned `desktop-app::lib_ui` toolkit slice. It is **not** a Telegram Desktop fork: no Telegram protocol, accounts, chats, or product screens. Owned code lives in `src/`; toolkit sources are fetched into ignored `.deps/` at build time.

- [ANATOMY.md](ANATOMY.md) — repository map and build graph
- [AGENTS.md](AGENTS.md) — contributor validation checklist
- [`cmake/desktop-app-toolkit-lock.json`](cmake/desktop-app-toolkit-lock.json) — exact dependency lock

## License

Copyright 2026 LingTai contributors.

LingTai Desktop is licensed under the GNU General Public License, version 3 or later. See [LICENSE](LICENSE).
