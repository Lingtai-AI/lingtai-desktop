# LingTai Desktop managed delivery

This document records the release-engineering contract behind the public
installation path in the [README](../README.md). It is intended for packagers,
release maintainers, and contributors working on the managed lifecycle.

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
`cli/` directory. The command supports `open` (also the no-argument default),
`foreground [-- APP_ARGS...]`, `version`, `doctor`, official
`update [--version X.Y.Z]`, paired local-artifact
`update --archive ... --manifest ...`, and explicit
`uninstall --version X.Y.Z` / `uninstall --all`. A byte-identical same-version
update is reverified and is an idempotent no-op; lower versions are refused.
Each numeric version component is limited to nine ASCII decimal digits.

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
claim.

The bootstrap does not use a DMG, install under `/Applications`, use sudo, change
PATH or shell profiles, clear quarantine, or bypass Gatekeeper. If
`$HOME/.local/bin` is not already on PATH, invoke the launcher by its full path.
Existing shared `.local`, `.local/bin`, and `.local/share` directory modes and
unrelated contents are preserved; only newly created shared parents and the
exclusively managed root use restrictive defaults. Uninstall preflights the
complete managed tree—including every installed version—before deleting anything
and refuses unknown, tampered, or substituted paths without a partial cleanup.

## Optional DMG experiment

`scripts/package-macos.py` and `scripts/verify-macos-package.py` remain available
for explicit diagnostic/release DMG experiments, including their own signing and
notarization gates. This is an optional alternative only: it neither defines nor
gates the primary archive publication route, and a DMG is not required or
accepted by the managed `lingtai-desktop` install/update path.
