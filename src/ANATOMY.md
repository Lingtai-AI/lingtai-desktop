# `src/` anatomy

`src/` is the Desktop's owned C++ surface, deliberately flat and deliberately
small: most source pairs are their own single-owner library target, with three
exceptions — `main.cpp` and `crl_integration.cpp` live only in the smoke
executable, and `native_shell.cpp` and the two
`ui/` widgets compile only into the shell library. The folder splits
into two kinds of code — **entry/composition/presentation** (the persistent
shell, its dialog, its widgets) and **domain readers/adapters** (pure models
and one-leaf-per-walk filesystem access). The split is enforced by CMake link
edges, not by convention: the readers link only what they need, and the shell
is the only owner that links them all together. See the repository map
[`../ANATOMY.md`](../ANATOMY.md) for the top-level build graph and the
`tests/` contracts; this file descends into the `src/` folder itself.

## Components

Entry point and composition root:

- `main.cpp` — `main()`: constructs `ShellHost` and runs the `--smoke` /
  `--offscreen` paths.
- `shell_host.{h,cpp}` — owns all native windows, the one process
  `DesktopStatusItem`, most-recent-active-window selection, directory-picker
  routing, and the fallback kernel interpreter. Final-window close still exits
  the application; the status item does not make Desktop resident.
- `desktop_status_item.{h,cpp}` — the narrow Qt adapter that owns one
  `QSystemTrayIcon`, its exact Show/separator/Quit menu, and the compiled
  18-point monochrome mask icon. Its two callbacks leave window selection and
  process lifetime policy in `ShellHost`.
- `mac_popup_dismissal_bridge.{h,mm}` — the macOS application-wide native
  event observer. It classifies mouse-down recipients by `NSWindow` identity
  against every visible top-level `Qt::Popup`, then invokes one synchronous
  callback without consuming or replaying the event. It retains no window,
  widget, popup, submenu, or event pointer.
- `native_shell.{h,cpp}` — the C5 composition owner: owns one `Ui::RpWindow`,
  the roster column, the content pane, native dialog orchestration, the
  composer's local slash-command/publisher dispatch, the one-second refresh
  timer, the application-owned pinned-toolkit emoji lifecycle, and the two
  click-armed pending observations.
  It also owns the `DirectMailboxSnapshotIndex` generation state and its
  cancellation token: the timer performs only a fixed-count mailbox-folder
  fingerprint, while one detached worker reads a shared all-route snapshot
  and posts a current-generation result back to the UI thread. Its
  project/Agent-keyed pending-publication projection merges publisher-proven
  outgoing rows with that accepted snapshot, maintains presentation append
  lineage, and retires a row only after the same ID is authoritative.
  The roster column is separated from the content pane by one semantic 8px
  drag handle (`lingtai_roster_resize_handle`, distinct from the one-pixel
  `Ui::PlainShadow` `lingtai_roster_separator` that follows it) whose drags
  re-derive a runtime-only 22%-30% roster width ratio over the absolute
  260px / 380px two-surface minima. Its composition seams are
  `set_agent_start_fallback_python` and the narrow
  `set_attachment_picker` injection used by shell tests (when unset, the
  product path opens the native dialog), plus `open_project` and the read-only
  `window()` / `selection_state()` accessors;
  `smoke_ready()` is real product readiness used only by `main.cpp`'s `--smoke`
  path (`native_shell.h:100`).
  `/setup` is its explicit rerun-existing mode: it loads and retains one
  `AgentSetupState`, presents Desktop's full saved/template catalog at Preset,
  uses the shared editor for real rows, hydrates reference-aware Agents/Review
  pages, fixes the selected Agent name/directory, and saves through
  `AgentSetupStore`; its narrow save-result injection exists only for
  deterministic shell tests.
  New Project reuses that catalog/wizard policy, runs the Desktop creation
  transaction asynchronously, and at this UI boundary alone maps exact `~` /
  `~/...` through a valid absolute `HOME` without evaluating other user input.
  A transaction rejection restores the same populated Review page and its
  visible typed error for correction/retry; publication attaches normally and
  hands first launch to `AgentLifecycleController`. Every entry resets the
  other setup mode.
  The content pane composes one coherent workspace: the no-project welcome
  branding and its rhythm spacing live in the empty route only (a selected
  project's route starts at the content origin), and the selected-Agent page
  nav is content-driven — two leading buttons plus one trailing positive-stretch
  spacer — with the duplicate Conversation heading retained only as a hidden
  object/implementation anchor (the nav item owns the user affordance).
- The shell retains two stable anchors for the selected-Agent chat top bar —
  `chat_top_bar_` (`lingtai_chat_top_bar`) and `selected_agent_key_`
  (`lingtai_selected_agent_key`) — so `recompute_layout`'s one responsive fit
  measure (`update_top_bar_fit` → `fit_selected_agent_chat_top_bar`) can derive
  the actual detail width (body minus the actual chosen roster width, 8px
  handle, and 1px separator in Normal mode; the body width in OneColumn
  detail), allocate remaining width to the identity column
  (`lingtai_selected_agent_identity`), and elide the presentation name plus
  the Sidebar-matching Role · Status line under it. The status row is never
  hidden for width; primary controls, fonts, and object names are untouched.
- `crl_integration.cpp` — the owned parent `crl` update producer: exactly one
  `crl::on_main_update_requests()` returning `rpl::never<>()` (no update
  source in the bounded smoke).
- `agent_detail_view.{h,cpp}` — selected-Agent presentation owner. Its
  composer holds the ordered pending `AcceptedAttachment` draft, wrapping
  cards/removal, semantic notice timer, and per-card send errors; it emits a
  picker request but never opens a dialog, resolves a route, or publishes.
- `attachment_thumbnail.{h,cpp}` — Qt image-preview helper for composer and
  history cards: reopens the observed regular file without following links,
  revalidates identity and size, rejects implausible decode dimensions or
  allocation, requests a bounded decoder size, and returns an empty pixmap for
  the normal file-card fallback. History previews do not apply send caps.

Domain models (pure, Qt-light state/derivation owners):

- `project_attachment.{h,cpp}` — Qt-independent project-root containment
  seam: `attach_project` canonicalizes a selected directory and
  `ProjectAttachment::resolve` verifies component-wise containment of a
  relative path.
- `attachment_selection.{h,cpp}` — Qt-independent selected-file preflight:
  canonical/opened regular-file facts, stable source metadata, filesystem-
  identity deduplication, the shared pure conservative filename classifier,
  ordered limits, and typed local rejections. It owns no UI, copy, or
  publication authority.
- `workspace_selection.{h,cpp}` — C1 model: the sole owner of the optional
  accepted active project and optional selected Agent directory key, and the
  sole same-root/root-switch transition owner.
- `agent_setup_store.{h,cpp}` — `AgentSetupState`/`AgentSetupDraft` plus
  `reconcile_agent_setup_presets` (`agent_setup_store.h:15-127`): the
  UI-independent existing-Agent setup domain. It retains full JSON values and
  owns one validated staged
  transaction across the selected Agent, descriptor-walked configured env leaf,
  and peer preset/orchestrator propagation. An empty allowed-list request does
  not replace peer preset policy.
- `preset_catalog.{h,cpp}` — bounded, deterministic Desktop-owned global
  saved/template catalog loader. It accepts an injected global root, returns
  exact `PresetEntry` source paths in TUI-equivalent order, treats missing
  directories as empty, types directory-read failure, and never writes.
- `posix_descriptor_primitives.{h,cpp}` — `posix_internal` seam: move-only
  descriptor/directory-stream ownership, shared read flags, `safe_leaf`, and
  one-leaf-at-a-time no-follow `openat`-based opens. Internal; links nothing;
  no domain policy.
- `project_creation.{h,cpp}` plus
  `project_creation_resources.{h,cpp}` — `create_project`, compiled
  Desktop-owned `en`/`zh`/`wen` first-boot content, and
  `ProjectCreationRunner`:
  selected-preset/draft no-follow prevalidation, one owned sibling stage,
  in-stage allowed-preset reconciliation, localized resolved `.prompt`,
  adaptive-or-byte-exact `comment.md`, bounded exact shape/content and
  exactly-one-orchestrator validation, typed stage/detail, fd-anchored rollback
  before or after ownership-marker removal, exclusive atomic `.lingtai`
  publication, and joinable queued exactly-once delivery to the UI thread with
  destruction-time suppression. It does not gate publication on runtime
  readiness.

Domain readers/projections (stateless, read-only, one source each):

- `agent_projection.{h,cpp}` — `project_agents`: one composite scan of the
  canonical root's `.lingtai` immediate children; manifest identity/role,
  heartbeat presence, and optional `.status.json` from one wall-clock sample.
- `direct_conversation_route.{h,cpp}` — `resolve_direct_conversation_route`:
  pure route derivation from an already-produced snapshot; performs no
  filesystem access.
- `slash_command.{h,cpp}` — `parse_slash_command`: pure composer-text
  classification matching the TUI leading-slash / first-ASCII-space boundary;
  owns no dispatch, widget, filesystem access, or side effect.
- `direct_conversation_history.{h,cpp}` — the descriptor-safe mailbox
  projection owner: `direct_mailbox_fingerprint` observes only the mailbox
  and three fixed folder leaves; `read_direct_mailbox_snapshot` scans each
  entry once and classifies it across every current Agent route; and
  `DirectMailboxSnapshotIndex` owns deterministic single-flight, generation,
  stale-result, in-scan-change, stable per-history revisions, and exact
  worker-classified append lineage decisions. Accepted snapshots are shared
  immutably so the synchronous UI render borrows a short-lived history view
  instead of copying it. `read_direct_conversation`
  remains the one-route compatibility projection.
- `direct_conversation_attachment_actions.{h,cpp}` — fresh action-time
  current-route/current-entry resolver and no-follow regular-file identity
  revalidation. It returns only the freshly reconstructed path and launches
  nothing.
- `agent_preset_summary.{h,cpp}` — `read_agent_preset_summary`: reads the
  kernel-published `system/manifest.resolved.json` v1 envelope plus an
  `init.json` mtime staleness comparison.
- `kanban_model.{h,cpp}` — full-reader compatibility plus the session-owned
  `KanbanSnapshotIndex`: per-source fingerprints/JSONL cursors, cached daemon
  inventory and completed summaries, rebuild-generation validation, atomic
  board composition, and deterministic payload/open metrics/test hook.
- `kanban_page.{h,cpp}` — Kanban presentation, including cold loading and
  nonblocking warm updating/stale status without clearing a complete board.

Direct-operation/side-effect owners (the only writers/launchers):

- `direct_mail_publisher.{h,cpp}` — `send_direct_mail`: revalidates accepted
  local-file identity/size/limits, exclusively creates one human
  `outbox/<id>` leaf, privately copies optional attachments, and publishes the
  temp-then-renamed `message.json` last; success returns only the stamped
  message and descriptor-established copied-file facts needed for immediate
  session presentation, while failures roll back that owned leaf.
- `agent_signal.{h,cpp}` — descriptor-relative/no-follow fixed-content
  lifecycle marker writer/remover shared by sleep and the controller.
- `agent_sleep.{h,cpp}` — sleep event baseline/observation plus the
  compatibility `.sleep` request wrapper.
- `agent_process.{h,cpp}` — platform adapter for exact runtime argv discovery
  and PID-revalidated TERM/KILL.
- `agent_launch.{h,cpp}` — secure configured-runtime resolution and detached,
  shell-free `<python> -m lingtai run <dir>` launch with PID/log outcome.
- `agent_lifecycle.{h,cpp}` — target and argument policy plus the timer-driven
  sleep/suspend/CPR/clear/hard-refresh state machine.
- `project_creation.{h,cpp}` — the initial-project writer described above;
  post-publication launch stays with `AgentLifecycleController`.
- `agent_setup_store.{h,cpp}` — the only existing-Agent configuration writer:
  descriptor-bounded load, setup-owned leaf patches, same-directory stage +
  atomic rename, and bounded rollback. It never scaffolds an Agent or writes
  UI/live runtime state.

Presentation widgets under `src/ui/` (their public seams are
`ui/agent_roster.h` and `ui/conversation_surface.h`; widget internals route
to the forthcoming sibling [`ui/ANATOMY.md`](ui/ANATOMY.md), while this file
keeps the parent summary):

- `ui/agent_roster.{h,cpp}` — `AgentRoster`: the persistent left list column
  (project identity header, Open/New Project actions, scrollable 62px rows);
  rebuilds its row tree only when the visible model changes. Its visible rows
  omit the human pseudo-agent: `project_agents` still emits the human row in
  the shared snapshot (routing, mailbox, and selected-detail truth consume
  it), and only the roster presentation filters it out.
- `ui/conversation_surface.{h,cpp}` — `ConversationSurface`: a read-only
  `QTextEdit` that renders the direct-conversation rows as plain-text
  blocks with palette-backed bubbles.

## Connections

- `main.cpp` → `NativeShell`: composes it, sets the two injectables, shows it;
  in smoke mode consumes `smoke_ready()` and emits the ordered markers.
- `NativeShell` → readers/owners: the shell is the sole caller of
  `project_agents`, `resolve_direct_conversation_route`,
  `parse_slash_command`, the shared mailbox fingerprint/snapshot projection,
  `send_direct_mail`,
  `read_agent_preset_summary`, `AgentLifecycleController`, and the
  `ProjectCreationRunner`, and `AgentSetupStore` for the selected-Agent
  `/setup` route. The click handlers rerun `project_agents`
  once at the click boundary and update the sole `agents_` snapshot.
- `NativeShell` → `WorkspaceSelectionState`: the shell proposes typed
  transitions (`activate_project`, `select_agent`,
  `clear_agent_selection`) and re-derives every visible route from the model;
  the model performs no reads.
- Readers → `posix_internal`: `agent_projection`, `direct_conversation_history`,
  `direct_mail_publisher`,
  `agent_preset_summary`, `agent_signal`, `agent_sleep`, `agent_launch`, and
  `agent_lifecycle` consume the shared
  descriptor primitives as a private dependency. `project_attachment` and
  `workspace_selection` do not (pure `std::filesystem`/state), while
  `attachment_selection` opens arbitrary caller-selected absolute sources
  directly because it is not anchored in a project tree.
- `direct_conversation_history` → `attachment_selection`: only the shared pure
  filename media classifier; history applies no outgoing selection limits.
- CMake link edges are the structural enforcement: `lingtai_desktop_direct_route`
  links only `lingtai_desktop_core` (a second discovery read is structurally
  impossible), and `lingtai_desktop_agent_launch` links only
  `lingtai_desktop_core` + `Qt6::Core` (no reader). The smoke executable links
  `lingtai_desktop_native_shell` + `desktop-app::lib_ui`.

## Composition

Owned library targets (`CMakeLists.txt`) and their source membership:

- `lingtai_desktop_core` — `project_attachment.cpp`, `workspace_selection.cpp`.
- `lingtai_desktop_attachment_selection` — `attachment_selection.cpp`.
- `lingtai_desktop_posix_primitives` — `posix_descriptor_primitives.cpp`.
- `lingtai_desktop_agent_projection` — `agent_projection.cpp`.
- `lingtai_desktop_direct_route` — `direct_conversation_route.cpp`.
- `lingtai_desktop_slash_command` — `slash_command.cpp`.
- `lingtai_desktop_conversation` — `direct_conversation_history.cpp`.
- `lingtai_desktop_mail_publisher` — `direct_mail_publisher.cpp`.
- `lingtai_desktop_agent_preset_summary` — `agent_preset_summary.cpp`.
- `lingtai_desktop_agent_setup_store` — `agent_setup_store.cpp`.
- `lingtai_desktop_preset_catalog` — `preset_catalog.cpp`.
- `lingtai_desktop_project_creation` — `project_creation.cpp`.
- `lingtai_desktop_agent_sleep` — `agent_sleep.cpp`.
- `lingtai_desktop_agent_launch` — `agent_launch.cpp`.
- `lingtai_desktop_agent_signal` — `agent_signal.cpp`.
- `lingtai_desktop_agent_lifecycle` — `agent_lifecycle.cpp`,
  `agent_process.cpp`.
- `lingtai_desktop_kanban` — `kanban_model.cpp`.
- `lingtai_desktop_native_shell` — `native_shell.cpp`,
  `mac_popup_dismissal_bridge.mm`,
  `agent_detail_view.cpp`, `attachment_thumbnail.cpp`,
  `ui/agent_roster.cpp`,
  `ui/conversation_surface.cpp`.
- `lingtai_desktop_smoke` (executable) — `main.cpp`, `crl_integration.cpp`.

`lingtai_desktop_native_shell` links `desktop-app::lib_ui` privately, links
`lingtai_desktop_core` publicly, and links every reader/owner library plus the
pure slash classifier privately. The two `ui/` widgets are compiled only into the shell library;
they are the shell's presentation layer and own no domain reads or writes.

## State

- `WorkspaceSelectionState` (C1) holds the optional accepted `ProjectAttachment`
  and the optional selected Agent directory key. It is the only in-process
  persistent model; a fresh open preserves selection only for the same
  canonical root (`workspace_selection.cpp:20`).
- `NativeShell` holds one `agents_` snapshot (the sole roster owner) and the
  two click-armed pending observations (`SleepObservation` at most 3 s,
  `StartObservation` at most 10 s). All are discarded on project open
  or selection change and are never persisted.
- `DesktopUiIntegration` owns the single process-lifetime
  `forcePopupMenuHideRequests()` event stream. When Desktop installs that
  Integration, its application-owned `MacPopupDismissalBridge` is installed
  once and every simultaneous or later shell shares it. A host-provided
  foreign `Ui::Integration` prevents Desktop from installing either object.
- `AgentDetailView` holds the session-only attachment draft and errors for the
  currently selected target. Project/Agent changes and route loss clear it;
  it is never persisted or shared across targets.
- `KanbanSnapshotIndex` keeps only in-session source cursors and completed-run
  summaries for the active project; project change discards them and nothing
  is persisted. Other readers remain independent stateless observations.
  Owned boundaries: the direct local leaf
  writers (`send_direct_mail`, `request_agent_sleep`) write only their own
  leaf; `start_agent` owns the Agent's log plus the detached launch; and
  `create_project` exclusively publishes the initial `.lingtai` tree from its
  bounded stage. `AgentSetupStore` alone may transactionally patch
  its declared setup fields in existing Agent configuration, the exact env leaf
  authorized by selected `init.json`, and bounded peer preset/orchestrator
  fields; it never creates an Agent.

## Notes

- `main.cpp` deliberately performs no `.lingtai` or Agent writes: its concrete
  choices are the fallback interpreter and open-project directory picker.
- The `posix_internal` seam is intentionally not a filesystem framework: it
  owns mechanics only (ownership, flags, one-leaf validation), and every
  bound, parser, candidate choice, and error mapping stays in the owning
  reader.
- `make_native_window()` installs the process-lifetime `base::Integration`,
  `Ui::Integration`, and animations manager before any vendored widget, starts
  palette and widget styles in order, then creates exactly one
  `DesktopEmojiRuntime` child of the current `QApplication`. That guard calls
  `Ui::Emoji::Init()` before any composer exists and `Ui::Emoji::Clear()` when
  the application deletes it after stack-owned shells. This is glue for the
  pinned toolkit, not composer business logic
  (`native_shell.cpp:1041-1067`, `native_shell.cpp:1153-1188`;
  stack construction order at `main.cpp:17-33`).
- On macOS, Desktop's installed `Ui::Integration` also creates exactly one
  `MacPopupDismissalBridge` parented to the current application. The bridge
  enumerates existing native popup windows only while handling a qualifying
  native mouse-down; `lib_ui` remains the sole owner of popup-tree closure and
  deferred deletion.
