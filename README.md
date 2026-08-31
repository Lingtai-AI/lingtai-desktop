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

Desktop owns project creation, preset setup, and Agent launch directly against
the shared on-disk model and managed kernel runtime.

## Requirements

- macOS 13.0 or newer (validated on macOS 15 in CI)
- CMake 3.25+, Ninja, Git, curl, Python 3, Xcode command-line tools
- Qt 6.11.1 macOS universal distribution
- A managed LingTai kernel runtime for Agent launch

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

## Portable App archive and terminal install

The Puffo-inspired product boundary is a thin terminal command plus a portable,
self-contained universal `LingTai.app`. The primary release artifact is a
versioned `.tar.gz` containing exactly one top-level `LingTai.app/`, paired with
an exact JSON manifest. This archive/manifest pair is the single primary public
release and installation contract; Developer ID signing, hardened runtime,
timestamping, notarization, stapling, and a DMG are not prerequisites for it.

Package an already verified App, then run the independent verifier:

```bash
python3 scripts/package-app-archive.py \
  --app build/LingTai.app \
  --output-dir /private/tmp/lingtai-desktop-package
python3 scripts/verify-app-archive.py \
  --archive /private/tmp/lingtai-desktop-package/LingTai-0.1.5-macOS-universal.app.tar.gz \
  --manifest /private/tmp/lingtai-desktop-package/LingTai-0.1.5-macOS-universal.app.manifest.json
```

The manifest binds the archive bytes, exact bundle identity/version/executable,
main executable bytes and universal architectures, recursive App-tree digest,
and packaging-checkout HEAD/tree/dirty facts. Packaging provenance is not
claimed as App-build provenance. The producer independently verifies the pair
before exclusively publishing it, and preserves any racer-created destination.
For the same numbered release, produce and independently validate the exact
support manifest/two-payload set into a new output directory:

```bash
python3 scripts/support_release.py build \
  --version 0.1.6 --output /private/tmp/lingtai-desktop-support-0.1.6
python3 scripts/support_release.py validate \
  /private/tmp/lingtai-desktop-support-0.1.6
```

Install exactly one versioned archive and its exact manifest from the latest
official stable `Lingtai-AI/lingtai-desktop` GitHub Release (or select an exact
stable version with `--version X.Y.Z`):

```bash
python3 scripts/install-macos-app.py
```

For offline/diagnostic use, preserve the explicit local pair:

```bash
python3 scripts/install-macos-app.py \
  --archive /path/to/LingTai-0.1.5-macOS-universal.app.tar.gz \
  --manifest /path/to/LingTai-0.1.5-macOS-universal.app.manifest.json
```

The downloaded archive and manifest are independently treated as untrusted.
Their exact names, version, bytes, and App facts must agree; extraction rejects
absolute/traversing or invalid names, escaping links, special
devices/FIFOs/sockets, duplicates, and extra top-level content. Verification and
smoke run in private disposable directories before atomic version publication
under the user-level managed root. The staged executable receives the exact
isolated environment and `--smoke` invocation, with a 60-second ceiling and
ordered ready/full-target marker requirements; timeout, exit, or marker failure
aborts publication.

The managed files are `$HOME/.local/bin/lingtai-desktop`, the App plane under
`$HOME/.local/share/lingtai-desktop/{versions,receipts,current,update-check.json}`,
and the independent support plane under
`$HOME/.local/share/lingtai-desktop/support/{versions,current,state.json,pending.json,update-check.json}`
(`pending.json` and the support cache are optional). There is no flat mutable
`cli/` directory. The command
supports `open` (also the no-argument default), `foreground [-- APP_ARGS...]`,
`version`, `doctor`, official `update [--version X.Y.Z]`, paired local-artifact
`update --archive ... --manifest ...`, and
explicit `uninstall --version X.Y.Z` / `uninstall --all`. A byte-identical
same-version update is reverified and is an idempotent no-op; lower versions are
refused. Each numeric version component is limited to nine ASCII decimal digits.
Normal commands validate syntax before consulting the private, single-link,
fixed-schema App and support caches on independent cadences. A newer support
release produces a noninteractive notice only; an interactive terminal defaults
No and only `y`/`yes` downloads all exact support assets, verifies their canonical
manifest sizes/hashes, publishes one pending generation transaction, and reexecs
the same command through the stable bootstrap. Ordinary support cache/network
errors print a truthful warning and continue without replacing prior cache bytes.
Explicit official `update` forces fresh support discovery/staging first and then
fresh App update, reporting both planes; a support failure may leave that plane
unchanged while App update continues. Paired local App updates perform no support
network or cache operation. The bootstrap itself remains local/no-network,
consumes the recursion marker before importing the selected generation, and
cannot stage twice. V1 support authenticity means TLS plus the exact official
GitHub repository/tag/asset route and manifest SHA-256 facts; it is not a signing
claim. The bootstrap
does not use a DMG, install under `/Applications`, use sudo, change PATH or shell
profiles, clear quarantine, or bypass Gatekeeper. If `$HOME/.local/bin` is not
already on PATH, invoke the launcher by its full path. Existing shared `.local`,
`.local/bin`, and `.local/share`
directory modes and unrelated contents are preserved; only newly created
shared parents and the exclusively managed root use restrictive defaults.
Uninstall preflights the complete managed tree—including every installed
version—before deleting anything and refuses unknown, tampered, or substituted
paths without a partial cleanup.

### Optional DMG experiment

`scripts/package-macos.py` and `scripts/verify-macos-package.py` remain available
for explicit diagnostic/release DMG experiments, including their own signing and
notarization gates. This is an optional alternative only: it neither defines nor
gates the primary archive publication route, and a DMG is not required or
accepted by the managed `lingtai-desktop` install/update path.

## First project

If you have no LingTai project yet, use **Create project** in the app. Desktop
discovers your saved/template presets, creates the first orchestrator, opens the
new project through the normal attach path, and starts its Agent.

## Architecture

LingTai Desktop is a native Qt app built on a pinned `desktop-app::lib_ui` toolkit slice. It is **not** a Telegram Desktop fork: no Telegram protocol, accounts, chats, or product screens. Owned code lives in `src/`; toolkit sources are fetched into ignored `.deps/` at build time.

- [ANATOMY.md](ANATOMY.md) — repository map and build graph
- [AGENTS.md](AGENTS.md) — contributor validation checklist
- [`cmake/desktop-app-toolkit-lock.json`](cmake/desktop-app-toolkit-lock.json) — exact dependency lock

## License

Copyright 2026 LingTai contributors.

LingTai Desktop is licensed under the GNU General Public License, version 3 or later. See [LICENSE](LICENSE).
