# `src/` anatomy

`src/` is the Desktop's owned C++ surface, deliberately flat and deliberately
small: most source pairs are their own single-owner library target, with three
exceptions — `main.cpp` and `crl_integration.cpp` live only in the smoke
executable, and `native_shell.cpp`, `project_bootstrap.cpp`, and the two
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

- `main.cpp` — `main()`: constructs one `NativeShell`, installs the two
  injectable dependencies application composition owns (the Desktop fallback
  interpreter and the configured TUI executable), wires the native directory
  picker, and runs the `--smoke` / `--offscreen` paths. Sole owner of the
  `lingtai-tui` PATH lookup and the `$HOME/.lingtai-tui/runtime/venv/bin/python`
  fallback (`main.cpp:13`).
- `native_shell.{h,cpp}` — the C5 composition owner: owns one `Ui::RpWindow`,
  the roster column, the content pane, the New Project dialog, the composer and
  its local slash-command dispatch, the one-second refresh timer, and the two
  click-armed pending observations.
  The roster column is separated from the content pane by one semantic 8px
  drag handle (`lingtai_roster_resize_handle`, distinct from the one-pixel
  `Ui::PlainShadow` `lingtai_roster_separator` that follows it) whose drags
  re-derive a runtime-only 22%-30% roster width ratio over the absolute
  260px / 380px two-surface minima. Public seams are the two setters
  (`set_tui_executable`, `set_agent_start_fallback_python`), `open_project`,
  and the read-only `window()` / `selection_state()` accessors;
  `smoke_ready()` is real product readiness used only by `main.cpp`'s `--smoke`
  path (`native_shell.h:100`).
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
- `posix_descriptor_primitives.{h,cpp}` — `posix_internal` seam: move-only
  descriptor/directory-stream ownership, shared read flags, `safe_leaf`, and
  one-leaf-at-a-time no-follow `openat`-based opens. Internal; links nothing;
  no domain policy.

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
- `direct_conversation_history.{h,cpp}` — `read_direct_conversation`: reads
  the human's own `mailbox` `inbox`/`sent`/`outbox` `message.json` rows and
  descriptor-validates ordered attachment metadata under each current entry.
- `agent_preset_summary.{h,cpp}` — `read_agent_preset_summary`: reads the
  kernel-published `system/manifest.resolved.json` v1 envelope plus an
  `init.json` mtime staleness comparison.

Direct-operation/side-effect owners (the only writers/launchers):

- `direct_mail_publisher.{h,cpp}` — `send_direct_mail`: revalidates accepted
  local-file identity/size/limits, exclusively creates one human
  `outbox/<id>` leaf, privately copies optional attachments, and publishes the
  temp-then-renamed `message.json` last; failures roll back that owned leaf.
- `agent_sleep.{h,cpp}` — `request_agent_sleep`: creates/truncates one
  `.sleep` marker; plus the best-effort `sleep_received` baseline/observe pair.
- `agent_launch.{h,cpp}` — `start_agent`: one detached, shell-free
  `<python> -m lingtai run <dir>` start with `logs/agent.log` redirection.
- `project_bootstrap.{h,cpp}` — `ProjectBootstrapRunner`: async, shell-free
  owner of the two headless TUI calls `<exe> presets` and
  `<exe> spawn <dir> --preset <name>` with exact separate argv.

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
  `parse_slash_command`, `read_direct_conversation`, `send_direct_mail`,
  `read_agent_preset_summary`,
  `request_agent_sleep` + the baseline/observe pair, `start_agent`, and the
  `ProjectBootstrapRunner` calls. The click handlers rerun `project_agents`
  once at the click boundary and update the sole `agents_` snapshot.
- `NativeShell` → `WorkspaceSelectionState`: the shell proposes typed
  transitions (`activate_project`, `select_agent`,
  `clear_agent_selection`) and re-derives every visible route from the model;
  the model performs no reads.
- Readers → `posix_internal`: `agent_projection`, `direct_conversation_history`,
  `direct_mail_publisher`,
  `agent_preset_summary`, and `agent_sleep` all consume the shared
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
- `lingtai_desktop_agent_sleep` — `agent_sleep.cpp`.
- `lingtai_desktop_agent_launch` — `agent_launch.cpp`.
- `lingtai_desktop_native_shell` — `native_shell.cpp`, `project_bootstrap.cpp`,
  `ui/agent_roster.cpp`, `ui/conversation_surface.cpp`.
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
- No reader or owner keeps durable cursors or ledgers; every read is one
  independent stateless observation. Owned boundaries: the direct local leaf
  writers (`send_direct_mail`, `request_agent_sleep`) write only their own
  leaf; `start_agent` owns the Agent's log plus the detached launch; and
  `ProjectBootstrapRunner` delegates the entire scaffold/config boundary to
  the TUI headless surface.

## Notes

- `main.cpp` deliberately performs no `.lingtai` or Agent writes: its only
  concrete choices are the fallback interpreter, the configured TUI
  executable, and the open-project directory picker.
- The `posix_internal` seam is intentionally not a filesystem framework: it
  owns mechanics only (ownership, flags, one-leaf validation), and every
  bound, parser, candidate choice, and error mapping stays in the owning
  reader.
- `NativeShell` installs three process-lifetime adapters before any vendored
  widget is constructed (`base::Integration`, `Ui::Integration`, and the
  animations manager) and starts the palette before the window is built
  (`native_shell.cpp:543-548`). This is glue for the pinned toolkit, not product
  logic.
