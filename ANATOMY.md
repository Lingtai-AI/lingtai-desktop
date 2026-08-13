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
src/agent_projection.{h,cpp}           one composite Agent discovery/role/presence/status projection
src/direct_conversation_route.{h,cpp}  pure direct route + human sender identity
src/direct_conversation_history.{h,cpp} read-only direct conversation rows
src/direct_mail_publisher.{h,cpp}      one exclusive human outbox leaf publisher
src/agent_activity.{h,cpp}             stateless bounded selected-Agent activity snapshot reader
src/agent_sleep.{h,cpp}                one selected-Agent .sleep marker write + best-effort applied observation
tests/posix_descriptor_primitives_test.cpp descriptor ownership/no-follow contract
tests/agent_projection_test.cpp        composite discovery/role/presence/status/no-write contract
tests/direct_conversation_route_test.cpp pure route/eligibility contract
tests/direct_conversation_history_test.cpp direct membership/order/no-write/containment contract
tests/direct_mail_publisher_test.cpp   publish envelope/nonoverwrite/containment contract
tests/agent_activity_test.cpp          activity binding/order/allowlist/bounds/skip contract
tests/agent_sleep_test.cpp             sleep write-targeting/baseline-attribution/containment contract
tests/native_shell_test.cpp            native shell semantics/geometry/no-write contract
tests/project_attachment_test.cpp      real C++ attachment/containment behavior contract
tests/test_native_shell.py             process persistence and smoke-order contract
tests/workspace_selection_test.cpp     workspace/Agent state behavior contract
tests/test_repository_contract.py      pinned toolkit provenance + tracked-artifact guard
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
project-attachment seam and workspace selection state.
The smoke links the native shell with `desktop-app::lib_ui`.
`src/crl_integration.cpp` supplies the bounded, no-emission parent update
producer the smoke needs; it is owned LingTai glue, not a Telegram model.

The `lingtai_desktop_native_shell` library owns one real `Ui::RpWindow`, uses
its real `Ui::RpWidget` body, and composes a bounded sidebar with a flexible
content region using native widget layouts. It holds one
`WorkspaceSelectionState`; the visible empty/project routes are derived from
that C1 model. Its Qt-free open seam accepts an explicit selected directory and
an optional project-relative Agent directory. It activates C1 only after safe
`.lingtai` validation, then reads one composite Agent snapshot. Valid and
malformed rows retain ordered manifest, role, and presence truth. Only valid
rows propose selection through C1; a row click is the sole selection entry
point, and highlight/detail are re-derived from C1. `NativeShell::smoke_ready`
is real product readiness used only by `main.cpp`'s `--smoke` path, not a
public test seam. Same-root refresh preserves only a still-valid selected key;
failed opens leave the prior project, roster, and selection intact while
showing a transient error. The shell uses static accessible/object names for
semantic tests, and its minimum/default sizing protects both layout regions.
Compatibility is not a Send authorization gate: the shell has no compatibility
probe, policy, or panel, and a valid selection is immediately eligible to
compose and send.

Application composition in `src/main.cpp` owns the real native directory
picker. Cancel is a no-op; a nonempty choice is passed to the shell with no
selected Agent. The shell performs no project, registry, or Desktop-state
writes beyond two explicit user-triggered actions: one composer send through
`send_direct_mail`, and one Request sleep marker write through
`request_agent_sleep`.

`WorkspaceSelectionState` is C1's sole owner of the optional accepted active
project and optional selected Agent directory key, and the sole same-root/
root-switch transition owner; C5 may propose only typed transitions through
it. Activation compares accepted canonical roots without filesystem access,
preserving Agent selection for the same root and clearing it for a different
root. Agent keys use discovery's safe one-component relative grammar and are
never interpreted as identity. The model performs no roster reads,
persistence, registry import, or project/settings writes, and retains state
if an accepted root is later deleted or unmounted.

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
close-on-exec, no-follow, nonblocking read flags, strict immediate-leaf
validation, and one-leaf-at-a-time descriptor-relative directory/regular-file
opening (with optional directory creation) anchored from an already-open
parent descriptor or, for the very first component of a walk, an absolute
root path. It links nothing: no Qt, no project model, and no reader. The seam
is internal and intentionally minimal — it is not a general filesystem
framework and holds no domain policy. Candidate selection, size bounds, error
mapping, observation order, and every parser stay with the reader that owns
them. Discovery, the direct-conversation reader, and the mail publisher each
consume the seam as a private dependency, so no consumer inherits it
transitively.

`project_agents` is the sole production Agent projection: one composite scan
of the canonical attachment root's real `.lingtai` directory and its
immediate real child directories, all from one shared wall-clock sample. It
returns one deterministically ordered vector of rows; there are no public
standalone discovery/roster-only wrappers, no parallel index-parallel
vectors, and no completeness flag. Discovery owns a
1 MiB per-manifest and per-status source limit, enforced by a bounded read on
the actual opened descriptor rather than file metadata. Only a regular,
non-symlink `.agent.json` within that limit can make any JSON object `valid`;
present unsafe, unreadable, oversized, invalid, or non-object sources remain
visible as a `malformed` or `unsafe` row with a concise typed diagnostic —
not source path, mtime, byte count, or observed-time provenance. Missing or
disappearing manifests are omitted entirely, and one unsafe or unreadable
candidate directory is invisible without hiding a later healthy sibling.
Lossless child names are the stable keys. The same successfully parsed JSON
object derives only the pure role: missing/null `admin` is human, any direct
Boolean `true` in an `admin` object is main, and every other present shape is
an agent; malformed or unsafe manifests remain unknown.

A valid human role is `alive_human` without opening `.agent.heartbeat`;
malformed/unsafe/unknown-role rows are `unknown` and also short-circuit. Main
and ordinary agents use the same descriptor-anchored, no-follow access, with
a bounded 128-byte heartbeat read. A heartbeat is live only when its
timestamp and `now` are finite, it is not future, and
`0 <= now - timestamp < 5.0`; future and non-finite values are invalid, and
display age is never negative or non-finite. Heartbeat content is accepted
only by an exact decimal grammar and converted through the classic C++
locale, independent of `LC_NUMERIC`, and rejects hexadecimal floats and range
failures. Optional `.status.json` is a separately bounded, descriptor-
anchored, no-follow runtime observation containing only typed state, running,
PID, progress, active-turn, and positive-window context fields; a malformed
or absent status simply leaves that row's status unset. There is no manifest/
status mtime-order or ID/state agreement relation, no device/inode generation
comparison, no re-verification after a read, and no health/winner verdict:
the collapsed projection reads each source exactly once and reports what it
found. The projection adds no UI, config/init/resolved reads, polling,
watcher, process query, command, repair, or project-tree write.

`resolve_direct_conversation_route` consumes one already-produced accepted
Agent snapshot, the attached canonical root, and the C1 selected directory
key. It performs no filesystem access and no manifest or status rediscovery:
its library links only `lingtai_desktop_core`, so a second discovery read is
structurally impossible rather than merely avoided by convention. It returns
an optional compact route: a route resolves only for an exact selected valid
non-human row with a present target manifest `agent_id` and address, exactly
one valid addressed human row, and human/target addresses that differ; any
other case is simply no route, with no typed failure or candidate evidence
retained. Current human and target directory keys and addresses are returned
separately and are route authority for later mailbox access only. The route
additionally carries the canonical project root and the target's manifest
`agent_id`, used only to anchor the mailbox path for the current call. The
minimal human sender card copies only accepted typed manifest fields, never
unknown manifest fields or raw JSON. Mailbox folders, messages, attachments,
publication, and Desktop app data remain outside this owner.

`read_direct_conversation` and `send_direct_mail` both reach
`.lingtai/<human key>/mailbox` through the shared `posix_internal`
descriptor-relative primitives with an anchored, per-operation walk: the
project root, `.lingtai`, the human directory, and `mailbox` are each opened
one leaf at a time with `O_NOFOLLOW`, so no intermediate symlink at any of
those components can redirect either a read or a write outside the project.
Only the current sender's own `mailbox`/`outbox` folders are created if
missing; `.lingtai` and the human directory are opened, never created.

`read_direct_conversation` turns one accepted route into the rows the
selected-Agent surface shows. It reads the immediate `inbox`, `sent`, and
`outbox` entries under the anchored mailbox and their immediate
`message.json` files, opened descriptor-relative and no-follow at every step.
It rejects a symlinked or non-regular entry or message, rejects a
`message.json` above 1 MiB (enforced on the actual read, not just metadata),
and writes nothing. The body field is the kernel's `message`, never `body`.
Order comes from the kernel's own timestamp precedence — `received_at` on
delivery, `sent_at` once outbox/<id> becomes sent/<id>, and `deliver_at`
while still pending — with the entry directory basename as both the
displayed ID and the deterministic tie-break. Membership is exactly envelope
based: one sender, one recipient written as a string or one-element array,
no CC, and an incoming identity `agent_id` that must match the selected
target when present. Mail for another conversation is simply absent; only an
unsafe, unreadable, or malformed entry increments the one generic skipped
count, and one bad entry never hides a valid neighbor. A duplicate outgoing
ID observed in both outbox and sent collapses onto the sent copy. Nothing
about delivery, processing, reply chains, unread state, or attachments is
read or inferred. `NativeShell` renders those rows in the selected-Agent
detail area as a read-only plain-text surface that never interprets markup,
refreshed on project open/refresh and selection change with no background
poller.

`send_direct_mail` publishes one plain-text human outbox entry for the
route's target: an exclusively created `outbox/<id>` leaf (`mkdirat`, retried
up to eight times on an id collision, matching the current Go TUI
pseudo-agent sender's own budget), then a write-temp-then-`renameat` of
`message.json` inside it with an explicit close-and-check before the rename.
The envelope carries the current interoperable schema only — `id`,
`_mailbox_id`, `from`, `to`, `cc`, `subject`, `message`, `type`,
`received_at`, and the accepted human identity fields — with no status,
`deliver_at`, thread/reply, receipt, version, fingerprint, provenance, or
attachment field ever added. On local failure only the held fresh leaf from
that attempt is removed; pre-existing content is never touched. It never
writes the target inbox or the human `sent/`, never stages outside `outbox/`,
and never retries or reuses an id across calls.

`read_agent_activity` is one stateless, read-only projection of the selected
Agent's own `<root>/.lingtai/<selected key>/logs/events.jsonl` — a distinct
source and surface from the mailbox conversation above, never merged into it.
Every call reads one independent bounded binary suffix (at most 512 KiB) of
the file on the actual read, discards a leading mid-record fragment when that
read did not begin at byte zero, and accepts only complete LF-terminated
top-level JSON objects; an unterminated final line is simply not visited.
Every walked path component — `.lingtai`, the selected key, and `logs` — is
opened one no-follow leaf at a time, and the final `events.jsonl` must be a
real regular file; a missing, unsafe, or unreadable source reduces only to
one coarse unavailable result, and nothing is ever written. It projects only
the public allowlist in file byte order, keeping at most the latest 100 rows:
nonblank `diary` text whose `hidden` is not `true` and whose `visibility` is
absent or exactly `public`; and a `tool_call` row (bounded name plus optional
bounded `tool_args.action`) completed in place by its matching `tool_result`
(reduced to success/error/unknown status and a finite nonnegative
`elapsed_ms`). Unknown types and excluded types (`thinking`, `text_input`,
raw tool args/results, and anything else) are silently filtered; only a
malformed, non-object, or invalid-UTF-8 complete row increments the one
coarse `skipped` count. `NativeShell` renders this snapshot in a separate
read-only plain-text panel below the conversation/composer, refreshed on the
same open/selection paths plus one simple one-second view-scoped `QTimer`
that re-invokes the same stateless reader — the only poller in the shell, and
not a background thread, filesystem watcher, or persisted cursor.

`request_agent_sleep` reproduces exactly the canonical local `sleep` marker
protocol for one selected Agent: it creates or truncates
`<accepted root>/.lingtai/<selected key>/.sleep` to zero bytes, walked
descriptor-relative and no-follow with the shared `posix_internal`
primitives from the accepted root through `.lingtai` to the selected key.
Neither `.lingtai` nor the selected key's own directory is ever created;
only the one `.sleep` leaf is created or truncated, and an existing
non-regular target at that exact leaf name is refused rather than replaced.
An existing marker is overwritten, matching the canonical coalescing,
non-queued, non-exactly-once semantics; there is no request ID, temp/lock
file, or directory creation of any kind. `capture_agent_sleep_event_baseline`
records the selected Agent's own `logs/events.jsonl` byte size and whether
its last byte was a newline immediately before that one write, and
`observe_agent_sleep_received` independently reopens the same log afterward
and reports only whether one complete, LF-terminated
`sleep_received(source="signal_file")` row appended strictly after that
boundary; a pre-existing partial tail line that only completes afterward is
discarded using the recorded newline bit rather than attributed. Neither
function is a poller, cursor service, or ledger; both are stateless,
best-effort, and read/write nothing else.

`NativeShell` composes one Request sleep button and one status label in the
selected-Agent detail area, positioned after Agent Activity and before the
low-level manifest/status facts. Desktop's own eligibility gate -- a valid
manifest, a main/agent role, the canonical strict `< 5.0 s` heartbeat
predicate, and a known current `.agent.json.state` other than
`asleep`/`suspended` -- is re-evaluated at the click boundary by rerunning
`project_agents` once and updating the sole `agents_` snapshot, never a
second roster owner. A successful write shows exactly "Sleep requested." and
disables the button; the pending observation then piggybacks on the existing
one-second `QTimer` (no new timer, thread, or watcher) for at most three
wall-clock seconds, after which the sole `agents_` snapshot is refreshed once
more and the button/status reflect the freshly projected current state.
Project open or Agent selection change discards any prior target's pending
observation and terminal status immediately, so a late result can never
surface under a different selection. This action never claims `queued`,
target acknowledgement, or "Agent is asleep" from the write or a timeout
alone.

Qt is external rather than fetched or committed. Configure resolves the exact
Qt 6.11.1 prefix from `QT_ROOT` or the documented `$HOME/Qt/6.11.1/macos`
default. Normal execution constructs and shows the native shell and schedules
no automatic exit. Explicit `--smoke` mode shows the same real shell off-screen,
checks the window/body/regions/empty route, emits readiness and success markers
in that order, and exits under a timeout. On macOS the pinned `RpWindow` helper
requires a Cocoa `NSView`, so the smoke uses Cocoa plus Qt's
`WA_DontShowOnScreen`; other platforms use Qt's offscreen plugin.
