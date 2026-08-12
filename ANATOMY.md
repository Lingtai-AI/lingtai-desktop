# Repository anatomy

```
CMakeLists.txt                         top-level source build graph
cmake/desktop-app-toolkit-lock.json    exact toolkit, third-party, Qt, and blob lock
scripts/bootstrap-deps.sh              verified local source/resource bootstrap
scripts/configure.sh                   Qt-aware CMake configure wrapper
scripts/build.sh                       target build wrapper
scripts/smoke.py                       bounded explicit native-shell smoke runner
src/main.cpp                           persistent app entry + explicit smoke exit
src/crl_integration.cpp                minimal parent `crl` update-stream integration
src/native_shell.{h,cpp}               native window/sidebar/content/empty-route owner
src/project_attachment.{h,cpp}         Qt-independent project-root containment seam
src/workspace_selection.{h,cpp}        pure Desktop-owned workspace selection state
src/agent_manifest_discovery.{h,cpp}   manifest discovery + roster implementation
src/agent_manifest_discovery_test_seam.h deterministic filesystem race/error seam
src/agent_roster_presence.h            pure ordered role/presence projection
src/agent_roster_presence_test_seam.h  keyed heartbeat and wall-clock seam
src/compatibility_probe.{h,cpp}        Qt Core-owned read-only compatibility probe
tests/agent_manifest_discovery_test.cpp discovery/no-write behavior contract
tests/agent_roster_presence_test.cpp    strict role/heartbeat behavior contract
tests/native_shell_test.cpp            native shell semantics/geometry/no-write contract
tests/project_attachment_test.cpp      real C++ attachment/containment behavior contract
tests/test_native_shell.py             process persistence and smoke-order contract
tests/workspace_selection_test.cpp     workspace/Agent state behavior contract
tests/test_project_attachment.py       dependency-free C++ contract compile/run harness
tests/test_workspace_selection.py      dependency-free workspace contract harness
tests/compatibility_probe_test.cpp     compatibility behavior/no-write contract
tests/test_repository_contract.py      focused tracked-tree and lock contract
```

`bootstrap-deps.sh` materializes ignored `.deps/` inputs in three groups:

- `.deps/src/` contains the exact desktop-app toolkit repositories
  (`lib_ui`, `lib_base`, `lib_rpl`, `lib_crl`, `codegen`, and `legal`).
- `.deps/third_party/` contains the exact header/source dependencies used by
  the full `lib_ui` target. `.deps/build/openssl/` contains locally built,
  universal static OpenSSL outputs.
- `.deps/cmake_helpers/` contains checksum-verified helper CMake files, and
  `.deps/tdesktop-resources/` contains the three checksum-verified upstream
  parent resource blobs needed by this pinned `lib_ui` build.

The top-level CMake graph creates the upstream dependency target names expected
by the full pinned `lib_ui` CMake target and adds its complete source tree
without patching it. The Qt-independent `lingtai_desktop_core` library owns the
project-attachment seam and workspace selection state. The separate
`lingtai_desktop_compatibility` library privately owns Qt Core JSON parsing.
The smoke links the native shell with `desktop-app::lib_ui`.
`src/crl_integration.cpp` supplies the bounded, no-emission parent update
producer the smoke needs; it is owned LingTai glue, not a Telegram model.

The `lingtai_desktop_native_shell` library owns one real `Ui::RpWindow`, uses
its real `Ui::RpWidget` body, and composes a bounded sidebar with a flexible
content region using native widget layouts. It holds one
`WorkspaceSelectionState`; the visible “No project open” route is derived from
that model. “Open Project…” emits one callback request and records only the
request count; it does not choose a path, attach a project, or mutate C1 state.
The shell uses static accessible names and object names for semantic tests, and
its minimum/default sizing protects both layout regions during resize.

`WorkspaceSelectionState` is C1's sole owner of the optional accepted active
project, optional selected Agent directory key, and in-memory Desktop recents;
C5 may propose only typed transitions through it. Activation compares accepted
canonical roots without filesystem access, maintains a deduplicated bounded
MRU, preserves Agent selection for the same root, and clears it for a different
root. Closing retains recents, while removing a recent never changes active
state. Agent keys use discovery's safe one-component relative grammar and are
never interpreted as identity. The model performs no compatibility or roster
reads, persistence, registry import, or project/settings writes, and retains
state if an accepted root is later deleted or unmounted.

`ProjectAttachment` accepts an existing directory and retains its canonical
(symlink-resolved) root path. Its `resolve` method accepts nonempty existing
relative paths only; empty and dot-only input is invalid. It rejects absolute
paths and every `..` component, canonicalizes the target, and verifies
component-wise containment so an in-project symlink cannot escape. A path below
a regular file is reported separately from a missing target. Both attachment
and resolution return `ProjectPathFailure` plus any underlying filesystem
error; their `noexcept` API performs no project-tree writes and has no Qt or
Telegram dependency.

`target_not_found` is not a containment verdict and must never be treated as
safe-to-create. Path canonicalization cannot detect that an in-project hard
link shares an inode with a file outside the project. The stored canonical root
is path-stable only, not inode-pinned: replacing the directory at that path can
change what a later resolution observes.

`discover_agent_manifests` examines only the canonical attachment root's real
`.lingtai` directory and its immediate real child directories.
Discovery owns a 1 MiB per-manifest source limit, enforced by streaming read rather
than file metadata. Only a regular, non-symlink `.agent.json` within that limit can
make any JSON object `Valid`; present unsafe, unreadable, nonregular, oversized
(`too_large`), invalid, or non-object sources remain visible with typed provenance.
Missing or disappearing manifests are
omitted. Lossless child names are the stable keys. The same successfully parsed
JSON object derives only the pure role: missing/null `admin` is human, any
direct Boolean `true` in an `admin` object is main, and every other present
shape is an agent; malformed or unsafe manifests remain unknown. Identity,
capability, status, and lifecycle projection remain deliberately excluded.
Root enumeration errors fail closed, results are sorted, and the scanner never
writes or follows root, child-directory, or manifest symlinks. Qt JSON remains
private to the discovery implementation.

`project_agent_roster` calls accepted manifest discovery once, preserves its
report and deterministic item order, then reads one wall-clock value for the
whole presence projection. A valid human is `alive_human` without opening
`.agent.heartbeat`; malformed/unsafe roles are unknown and also short-circuit.
Main and ordinary agents use descriptor-anchored, no-follow access beneath the
same opened `.lingtai` root, with a streaming 128-byte heartbeat ownership
limit. A heartbeat is live only when its timestamp and `now` are finite, it is
not future, and `0 <= now - timestamp < 5.0`; future and non-finite values are
invalid, exact age five is stale, and display age is never negative or
non-finite. Typed heartbeat provenance keeps absence, unsafe sources, I/O,
container replacement, invalid numbers, and staleness independent from the
manifest lifecycle. The snapshot reads no status, performs no writes, retry,
process query, or repair, and retains discovery evidence if projection fails.

`probe_compatibility` reads one explicitly requested relative agent directory
and one explicitly supplied global install-receipt path below an accepted
attachment; it never infers `$HOME` or a project receipt. It recognizes only
the current machine receipt and kernel-resolved-manifest envelopes, reports raw
`init.json` structure and the `bash`/`shell` alias case without rewriting it,
and retains independent typed findings. No requested agent is explicitly
`Degraded`; commands are allowed only for a finding-free requested agent with a
recognized receipt, fresh resolved manifest, and structurally usable raw init.
The probe does not implement the kernel's full raw-init semantics or verify an
installed executable.

Qt is external rather than fetched or committed. Configure resolves the exact
Qt 6.11.1 prefix from `QT_ROOT` or the documented `$HOME/Qt/6.11.1/macos`
default. Normal execution constructs and shows the native shell and schedules
no automatic exit. Explicit `--smoke` mode shows the same real shell off-screen,
checks the window/body/regions/empty route, emits readiness and success markers
in that order, and exits under a timeout. On macOS the pinned `RpWindow` helper
requires a Cocoa `NSView`, so the smoke uses Cocoa plus Qt's
`WA_DontShowOnScreen`; other platforms use Qt's offscreen plugin.
