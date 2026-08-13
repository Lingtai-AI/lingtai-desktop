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
src/agent_task_card.{h,cpp}            stateless read-only selected-Agent Task Card status/body reader
src/agent_preset_summary.{h,cpp}       stateless read-only selected-Agent kernel-resolved Presets policy/effective reader
src/local_preset_draft.{h,cpp}         opaque project+Agent local preset draft store (app-data root, atomic replace)
src/agent_sleep.{h,cpp}                one selected-Agent .sleep marker write + best-effort applied observation
src/agent_launch.{h,cpp}               one selected-Agent detached, shell-free `lingtai run` start
tests/posix_descriptor_primitives_test.cpp descriptor ownership/no-follow contract
tests/agent_projection_test.cpp        composite discovery/role/presence/status/no-write contract
tests/direct_conversation_route_test.cpp pure route/eligibility contract
tests/direct_conversation_history_test.cpp direct membership/order/no-write/containment contract
tests/direct_mail_publisher_test.cpp   publish envelope/nonoverwrite/containment contract
tests/agent_activity_test.cpp          activity binding/order/allowlist/bounds/skip contract
tests/agent_task_card_test.cpp         Task Card active/inactive/containment contract
tests/agent_preset_summary_test.cpp    resolved/stale/unavailable Presets summary contract
tests/local_preset_draft_test.cpp      draft round-trip/isolation/oversize-preserves/no-project-write contract
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
picker and the one concrete Desktop fallback interpreter passed to
`set_agent_start_fallback_python`. Cancel is a no-op; a nonempty choice is
passed to the shell with no selected Agent. The shell performs no project,
registry, or Desktop-state writes beyond three explicit user-triggered
actions: one composer send through `send_direct_mail`, one Request sleep
marker write through `request_agent_sleep`, and one Start Agent detached
launch through `start_agent`, which also creates the selected Agent's own
`logs/` directory so its redirected `agent.log` has somewhere to land.

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

`read_agent_task_card` is one stateless, read-only projection of the selected
Agent's own two channel-facing artifacts,
`<root>/.lingtai/<selected key>/taskcard/status` and, conditionally,
`taskcard/taskcard.md` -- a distinct source and surface from both the
mailbox conversation and Agent Activity above, never merged into either.
Every walked path component -- `.lingtai`, the selected key, and `taskcard`
itself -- is opened one no-follow leaf at a time, and both leaves must be
real regular files; a missing, unsafe, or unreadable component reduces only
to the coarse `unavailable` state, and nothing is ever written. `status` is
read completely and bounded (32 bytes), and accepted only as the exact bytes
`active` or `inactive` -- no trimming, case folding, or inference; any other
content is `unavailable`. Only when `status` is exactly `active` does it
read `taskcard.md`, completely and bounded to a small independent 8 KiB
Desktop cap (not read from producer config) that comfortably covers the
current default 2,000-character producer body; a file over that bound is
refused rather than truncated. The body must be valid UTF-8 -- proven by an
exact byte round trip through `QString::fromUtf8`/`toUtf8()` rather than a
hand-rolled decoder -- and nonblank ignoring surrounding whitespace; a
blank or invalid body with an exact `active` status is also `unavailable`.
A valid nonblank body renders completely unchanged, with no Markdown, HTML,
or link execution. `NativeShell` composes one heading, one read-only
plain-text surface, and one compact state label in the selected-Agent
detail area, positioned after Agent Activity and before Start Agent,
refreshed on the same project-open/selection paths plus the existing
one-second `QTimer` -- no new timer, thread, or watcher. It holds one tiny
same-target `task_card_last_valid_` view state: an `unavailable`
observation for the currently selected target preserves and re-shows the
last valid active or inactive projection rather than clearing or erroring
it, while an exact `inactive` observation, a project open, or an Agent
selection change immediately clears any preserved active body -- so a late
observation can never surface under a different selection, matching the
independent Telegram/Feishu reference consumers' own no-op-on-unavailable
rule. The surface and state label are repainted only when the accepted
visible text or compact state actually changes. This slice never writes
`taskcard/status` or `taskcard/taskcard.md`, never reads `taskcard.json` or
`watch.json`, and never calls `TaskCardManager`.

`read_agent_preset_summary` is one stateless, read-only projection of the
selected Agent's own kernel-published
`<root>/.lingtai/<selected key>/system/manifest.resolved.json`
(`schema=lingtai.manifest.resolved/v1`, `source=kernel`) -- a distinct
source and surface from the mailbox conversation, Agent Activity, and Task
Card above, never merged into any of them, and never dereferencing an
allowed preset ref. Every walked path component -- `.lingtai`, the selected
key, and `system` -- is opened one no-follow leaf at a time, and the final
leaf must be a real regular file bounded by a fixed 1 MiB Desktop cap (a
source at or under the observed size that exceeds the bound is refused, not
truncated). A missing `system` component or `manifest.resolved.json` leaf
projects `not_yet_published`; a symlinked or non-regular component at
either exact name projects `unavailable`, not followed. A read artifact is
parsed only for the exact supported v1 envelope and narrow allowlisted
paths -- root `schema`/`schema_version`/`source`/`generated_at`, root
`preset.active`/`default`/`allowed` (exact strings and published order
preserved), `manifest.llm.provider`/`.model`, `manifest.context_limit`, and
top-level names from `manifest.capabilities` (sorted only for stable
display) -- and any missing or wrong-typed field refuses the whole artifact
as `unavailable` rather than a partial projection. Each allowed ref carries
independent `is_active`/`is_default` badges by exact string equality
against that same artifact's own active/default refs. `init.json` is
descriptor-opened only for one regular-file mtime comparison against the
artifact's own mtime -- never read or parsed, and never a fallback source
-- and a supported artifact strictly older than that mtime projects
`stale` instead of `resolved`; a missing/unsafe `init.json` simply yields
no staleness evidence. `NativeShell` composes one heading, one read-only
plain-text surface, and one compact state label ("Resolved" / "Not yet
published" / "Stale" / "Unavailable") in the selected-Agent detail area,
positioned after Task Card and before Start Agent, refreshed on the same
project-open/selection paths plus the existing one-second `QTimer` -- no
new timer, cache, or watcher. Unlike Task Card, there is no last-valid
preservation: every observation is shown exactly as read, so an absent,
stale, or unavailable current observation never keeps a prior target's
projection visible. The surface and state label are repainted only when
the accepted visible text or compact state actually changes. This slice
never opens an allowed ref, never reads or parses `init.json` content,
never calls a live Agent tool or `system(action="presets")`, and never
writes anything.

`load_local_preset_draft`/`save_local_preset_draft` own the Local preset
draft store: one bounded (64 KiB encoded), opaque, user-authored plain-text
document per exact (canonical project root, selected Agent's own nonempty
stable `agent_id`) key -- never a directory name, nickname, address, or
preset ref, so a directory rename that preserves `agent_id` keeps the same
draft while a moved project root is simply a different key. It is the
shell's first writer outside a real project tree: persisted only beneath an
explicitly injected Desktop application-data root (production resolves this
from Qt's application-data location after `main.cpp` sets one stable
application identity; focused/native tests inject a disposable temporary
root), inside one app-owned, owner-restrictive `local-preset-drafts`
subdirectory created only by a save, never a load. Each key maps to one
deterministic SHA-256 digest filename -- a lookup convenience only, never
the identity check itself, since `load` always re-verifies the envelope's
own recorded `project`/`agent_id` fields against the exact requested key
before trusting its `document`. A save is refused, never truncated, over
the 64 KiB bound, and otherwise always lands by one write-temp-then-
`renameat` atomic replace so a refused or failed save never touches
previously saved content. Desktop is the sole writer of this private root
in this slice: no lock, revision, history, migration, recovery ledger,
watcher, cache, or generic settings framework is added, and this module
never reads, parses, or dereferences any preset/config/credential source --
it treats `document` as opaque bytes throughout.

`NativeShell` composes one Local preset draft surface directly under the
read-only Presets summary: a heading, one explanation carrying both the
exact `Stored only by Desktop. Not active or applied.` meaning and a
concise credentials warning, one editable plain-text surface, Save
Draft/Discard Changes buttons, and one compact state label (no stable
identity / clean local draft / unsaved changes / saved locally / save
failed). A row with no nonempty stable `agent_id` disables the whole
surface with a truthful reason rather than falling back to a directory key.
The working copy loads from the store only on a genuine (project, agent_id)
target change -- tracked by one small memoized key -- never on an unrelated
same-target reroster and never from the existing one-second `QTimer`, which
never calls this renderer at all, so a same-selection background refresh
can never clobber an in-progress edit. Any text difference from the last
successfully loaded/saved value is dirty; only a successful Save Draft
updates the clean baseline, and Discard Changes restores the last loaded/
saved value without touching the saved file. A dirty Agent-selection change
and a dirty project change (`open_project`) share one guard function
implementing the same three-way Save Draft/Discard/Cancel boundary: Cancel
(the default real boundary is one blocking `QMessageBox`, overridable via
`set_local_preset_draft_boundary_prompt` for tests) or a failed Save leaves
the current selection/project and the dirty working text exactly as they
were, matched by a new `ProjectOpenDisposition::cancelled`; Discard always
restores the working copy to its last loaded/saved value before permitting
the transition, even when it happens to resolve back to the same target.
A dirty window close shares the exact same guard. `RpWidget::event()` is
`final` in the pinned toolkit, so `NativeShell` installs one private,
cpp-local `QObject` event filter (`LocalPresetDraftCloseGuard`) on the
existing concrete `window_` instead of adding an `RpWindow` subclass: on
`QEvent::Close` it calls `guard_local_preset_draft_transition()`, ignoring
the event (leaving the window open) on Cancel or a failed Save and letting
it pass through unmodified otherwise, so a clean draft, a successful Save,
or Discard closes exactly as before. This is the same primitive `RpWindow`'s
own macOS close() helper uses to deliver its `QCloseEvent`, so the filter
sees every platform's real close request. This surface never reads or
dereferences an allowed/active preset file, never imports/exports, never
offers Apply/Switch/Refresh, and never writes any project or Agent tree.

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

`start_agent` is the Step-6 leaf: given an already-accepted attachment, the
exact selected directory key, and application composition's one Desktop
fallback interpreter, it resolves the absolute selected Agent directory
through the existing `ProjectAttachment::resolve` containment seam, then
starts exactly `<python> -m lingtai run <absolute-selected-agent-dir>`
detached and without a shell via `QProcess`. The interpreter is the
selected Agent's own top-level `init.json.venv_path` platform Python --
read by one ordinary, bounded, non-descriptor-relative config read, since
the directory is already contained and the spawned kernel remains the
validating authority for its own config -- when that exact absolute path's
`bin/python` file exists, otherwise the fallback; a relative `venv_path` is
rejected rather than resolved against some ambient working directory, and a
present-but-broken configured interpreter is attempted rather than silently
replaced. This slice never provisions, installs, upgrades, import-probes,
or repairs either interpreter. Stdout/stderr are redirected to the selected
Agent's own `logs/agent.log` (creating `logs/` first if absent), matching
the current TUI launcher's own redirection, since the kernel process itself
never creates that file. `start_agent` tracks no PID, never waits on or
signals the spawned process, and returns only accepted/refused; the kernel
child remains the sole authority for config validation, duplicate defense,
workdir lease, signal cleanup, and lifetime.

`NativeShell` composes one Start Agent button and one status label,
positioned in the selected-Agent detail area before the Request sleep row.
Desktop's own eligibility gate -- a valid manifest, a main/agent role, and
exactly a stale or missing heartbeat (never `invalid`/`unavailable`, since
only those two presence kinds honestly exclude a false "online" reading
from wall-clock movement or a transient read failure with no genuine new
heartbeat write) -- is re-evaluated at the click boundary by rerunning
`project_agents` once, exactly like Request sleep. A live selection shows no
Start action at all: the button is hidden, not merely disabled. A
successful local start shows exactly "Starting Agent..." and disables the
button; Request sleep needs no separate disabling here since it already
requires `alive` presence, which a start-eligible row cannot have at that
instant. The pending observation then piggybacks on the existing one-second
`QTimer` (no new timer, thread, second scanner, or heartbeat parser) for at
most ten wall-clock seconds, succeeding only when the sole `project_agents`
projection reports the exact selected Agent `alive`, and showing "Agent is
online." with normal controls returning. A timeout shows "Agent did not
come online. See `<agent>/logs/agent.log`." and leaves the button enabled
for an explicit retry; a local start refusal shows "Could not start Agent.
See `<agent>/logs/agent.log`." immediately, with no pending observation.
Project open or Agent selection change discards any prior target's pending
observation and terminal status immediately -- never the detached process
itself, which this shell has no PID to kill or cancel -- so a late result
can never surface under a different selection. Start Agent never auto-
starts any Agent on its own, never claims readiness beyond heartbeat
liveness, and never retries, scans processes, or manages a launch ID.

Qt is external rather than fetched or committed. Configure resolves the exact
Qt 6.11.1 prefix from `QT_ROOT` or the documented `$HOME/Qt/6.11.1/macos`
default. Normal execution constructs and shows the native shell and schedules
no automatic exit. Explicit `--smoke` mode shows the same real shell off-screen,
checks the window/body/regions/empty route, emits readiness and success markers
in that order, and exits under a timeout. On macOS the pinned `RpWindow` helper
requires a Cocoa `NSView`, so the smoke uses Cocoa plus Qt's
`WA_DontShowOnScreen`; other platforms use Qt's offscreen plugin.
