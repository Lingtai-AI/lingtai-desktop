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
src/native_shell.{h,cpp}               native project/Agent roster selection route owner
src/project_attachment.{h,cpp}         Qt-independent project-root containment seam
src/workspace_selection.{h,cpp}        pure Desktop-owned workspace selection state
src/posix_descriptor_primitives.{h,cpp} internal descriptor/no-follow primitives
src/agent_manifest_discovery.{h,cpp}   manifest discovery + roster implementation
src/agent_manifest_discovery_test_seam.h deterministic filesystem race/error seam
src/agent_roster_presence.h            pure ordered role/presence projection
src/agent_roster_presence_test_seam.h  keyed heartbeat and wall-clock seam
src/agent_identity_status.h            manifest/status composite read model
src/agent_identity_status_test_seam.h  keyed status/mtime/clock seam
src/direct_conversation_route.{h,cpp}  pure direct route + human sender identity
src/compatibility_probe.{h,cpp}        Qt Core-owned read-only compatibility probe
tests/posix_descriptor_primitives_test.cpp descriptor ownership/no-follow contract
tests/agent_manifest_discovery_test.cpp discovery/no-write behavior contract
tests/agent_roster_presence_test.cpp    strict role/heartbeat behavior contract
tests/agent_identity_status_test.cpp    source-separated identity/status contract
tests/direct_conversation_route_test.cpp pure route/stable-identity contract
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
`WorkspaceSelectionState`; the visible empty/project routes are derived from
that C1 model. Its Qt-free open seam accepts an explicit selected directory,
install receipt, and optional project-relative Agent directory. It attaches
and probes through C2, activates C1 only after safe `.lingtai` validation, then
reads one C3 roster snapshot. Valid and malformed rows retain ordered manifest,
role, presence, completeness, and diagnostic truth. Only valid rows propose
selection through C1; row clicks and the public seam share one handler, and
highlight/detail are re-derived from C1. A successful selection re-probes C2
at `.lingtai/<directory_key>` using the retained in-memory receipt path.
Same-root refresh preserves only a still-valid selected key; failed opens leave
the prior project, receipt, roster, selection, and compatibility report intact
while showing a transient error. The shell uses static accessible/object names
for semantic tests, and its minimum/default sizing protects both layout regions.

Application composition in `src/main.cpp` owns the real native directory
picker. Cancel is a no-op; a nonempty choice is passed to the shell with the
current user's absolute `.lingtai-tui/install.json` path and no selected Agent.
The shell and compatibility probe perform no project, receipt, registry, or
Desktop-state writes.

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

`lingtai_desktop_posix_primitives` owns the internal POSIX mechanics that every
descriptor-anchored reader must not re-derive: move-only descriptor ownership
that closes exactly once and leaves a moved-from owner empty, ownership of a
directory stream that has adopted a descriptor, the shared read-only,
close-on-exec, no-follow, nonblocking read flags, device-plus-inode file
identity, platform nanosecond mtime conversion, and strict immediate-leaf
validation. It links nothing: no Qt, no project model, and no reader. The seam
is internal and intentionally minimal — it is not a general filesystem
framework and holds no domain policy. Candidate selection, size bounds, error
mapping, observation order, race verdicts, and every parser stay with the
reader that owns them; discovery keeps its own stat-versus-stat replacement
checks because those are discovery's race policy, not shared mechanics.
Discovery consumes the seam as a private dependency, so no consumer inherits it
transitively.

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
shape is an agent; malformed or unsafe manifests remain unknown.
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
Heartbeat content is accepted only by an exact decimal grammar and converted
through the classic C++ locale. This keeps parsing independent of `LC_NUMERIC`,
rejects hexadecimal floats and range failures, and preserves the typed
non-finite/future policy.

`project_agent_identity_status` extends that same C3 owner without a second
scan: one discovery and injected wall-clock value produce an index-parallel
roster/detail result. Valid manifest identity owns true name, nickname,
address, manifest state, safelisted live-LLM values, and raw/display capability
evidence. Optional `.status.json` is a separately bounded 1 MiB, descriptor-
anchored, no-follow runtime observation containing only typed state, running,
PID, progress, active-turn, and positive-window context fields. Each source
retains relative path, descriptor mtime, shared observation time, byte/read/
parse outcome, and error evidence. Mtime order plus optional Agent-ID/state
agreement is deterministic pair evidence only: status never replaces manifest
identity/state and no TTL, generation, winner, activity aggregate, or health
verdict is invented. The projection adds no UI, config/init/resolved reads,
polling, watcher, process query, command, repair, or project-tree write.

`resolve_direct_conversation_route` consumes one already-produced accepted
identity snapshot, the attached canonical root, and the C1 selected directory
key. It performs no filesystem access and no manifest or status rediscovery:
its library links only `lingtai_desktop_core`, so a second discovery read is
structurally impossible rather than merely avoided by convention. A completed
projection, exactly one valid human-role row with typed identity, and an
exactly matching selected valid non-human target are all required; every other
outcome is one precise typed `DirectRouteFailure` carrying bounded human
candidate keys and an exact candidate count for degraded presentation. Current
human and target directory keys and addresses are returned separately and are
route authority for later mailbox access only. Stable Desktop thread identity
is the transparent `DirectThreadKey` of canonical project root plus the
target's manifest `agent_id`; it is never hashed, never derived from an address
or directory key, and therefore survives a rename, while remaining exactly as
stable as the canonical root path itself. The minimal human sender card copies
only accepted typed manifest fields plus explicit human-role evidence, never
unknown manifest fields or raw JSON. Mailbox folders, messages, receipts,
attachments, publication, and Desktop app data remain outside this owner.

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
