# `src/` contract

This is the contract for the owned `src/` surface: it defines the stable
ownership and public-boundary rules an agent must respect before touching any
file here. It complements the repository contract and the sibling docs in the
root [`../ANATOMY.md`](../ANATOMY.md) (structure) and
[`src/BEHAVIORS.md`](BEHAVIORS.md) (current observable behavior). Anatomy
answers where code lives; this file answers who owns what and what each owner
promises.

## Behavior

Obligations and prohibitions for agents working in `src/`:

1. Read the paired [`src/ANATOMY.md`](ANATOMY.md) and this contract before
   editing any `src/` file, and update the pair in the same change as the
   code they describe.
2. Respect the single-owner rule: each behavior has exactly one owner.
   Do not add a second reader, a second projection, or a second writer for an
   already-owned source; extend the existing owner or, for a genuinely new
   source, introduce a new single-owner library target with its own contract
   test.
3. Do not let a UI widget absorb domain or business behavior. A widget may
   call an owner, render a snapshot, and hold view-only state; it must not
   re-derive project/Agent truth, implement eligibility policy, or write
   project files itself.
4. Keep the shell's injectable seams narrow: application composition
   (`ShellHost`) is the only place that chooses the concrete fallback
   interpreter and the configured TUI executable, and it passes them in
   through the two setters. `make_native_window()` is the application boundary
   for pinned toolkit prerequisites: it initializes process-global emoji state
   once after widget styles and keeps it alive until `QApplication` teardown.
   Do not hard-code either executable in the shell or any reader, and do not
   move toolkit lifecycle ownership into an individual widget or window.
5. Report a mismatch between this contract and the code rather than silently
   rewriting the promise to match accidental behavior.

## Port

The boundary an external caller (mainly `NativeShell` and `main.cpp`) uses is
a small set of free functions and small value objects. Compatibility read
functions are `noexcept`, stateless observations; the mailbox and Kanban
indexes are explicit in-memory generation/source state; every side-effecting
function returns one coarse result enum.

Reads (no writes, no durable state):

- `resolve_tui_executable(search)` → executable path or empty
  (`tui_executable_resolver.h`) — host-independent resolution policy over
  injected PATH/home/system roots; it never mutates PATH or starts a process.
- `project_agents(attachment)` → `AgentSnapshot` (`agent_projection.h`).
  The snapshot is shared live truth and keeps the human pseudo-agent row
  (routing, mailbox, and selected-detail truth consume it); only the
  `AgentRoster` presentation omits that row.
- `resolve_direct_conversation_route(attachment, snapshot, key)` →
  `std::optional<DirectConversationRoute>` (`direct_conversation_route.h`).
- `parse_slash_command(raw_text)` → `std::optional<SlashCommand>`
  (`slash_command.h`) — classification only; support and effects remain outside.
- `read_direct_conversation(route)` → `DirectConversationHistory`
  (`direct_conversation_history.h`) — text rows plus ordered, descriptor-
  validated attachment metadata (including device/inode identity and current
  mailbox folder) and independent attachment-skip accounting.
- `direct_mailbox_fingerprint(request)` → `DirectMailboxFingerprint` and
  `read_direct_mailbox_snapshot(request)` → `DirectMailboxSnapshot`
  (`direct_conversation_history.h`) — respectively a fixed-count folder
  observation with no entry enumeration, and one shared scan that parses each
  entry once before classifying it across all requested routes.
- `revalidate_direct_conversation_attachment(route, request)` → optional
  current path (`direct_conversation_attachment_actions.h`) — refreshes the
  current message entry, descriptor-walks its current `attachments/` directory
  no-follow, and yields a path only when index/name/device/inode/size still
  match the presentation-time observation.
- `read_agent_preset_summary(attachment, key)` → `AgentPresetSummary`
  (`agent_preset_summary.h`).
- `load_preset_catalog(global_dir)` → `PresetCatalogLoadResult`
  (`preset_catalog.h`) — bounded read-only saved/template discovery under the
  injected Desktop global root; missing directories are empty and directory
  read failure is typed. It writes and bootstraps nothing.
- `KanbanSnapshotIndex::refresh(attachment, snapshot, force)` →
  `KanbanRefreshResult` (`kanban_model.h`) — the session-only complete-board
  owner. Cold/forced reads may rebuild; unchanged refreshes perform fixed
  per-Agent metadata checks, JSONL appends advance rotation-safe cursors, and
  daemon membership reuses completed-run summaries. Rebuilds validate source
  stamps across the full-read/cursor-capture boundary and expose a follow-up
  until the affected Agent has one coherent generation. Capture incapability
  without generation movement remains dormant until that source stamp changes.
- `preflight_attachments(selected_paths)` → `AttachmentSelectionResult`
  (`attachment_selection.h`) — current canonical source paths, display names,
  sizes, device/inode identities, media kinds, accepted-byte accounting, and
  ordered typed rejections. It writes nothing and is never publication
  authorization.

Side-effecting operations (the only writers/launchers; each owns one explicit
bounded side-effect scope):

- `send_direct_mail(route, text, attachments = {})` → `DirectMailSendOutcome`
  (`direct_mail_publisher.h`) — one atomic human `outbox/<id>` leaf, with
  typed local failure facts and, only on success, the exact published
  id/timestamp/body plus descriptor-established copied-attachment metadata.
- `request_agent_sleep(attachment, key)` → `AgentSleepRequestResult`,
  plus `capture_agent_sleep_event_baseline` / `observe_agent_sleep_received`
  (`agent_sleep.h`) — one `.sleep` marker.
- `start_agent(attachment, key, fallback_python)` → `AgentLaunchResult`
  (`agent_launch.h`) — owns the Agent's `logs/agent.log` plus the detached
  launch.
- `ProjectBootstrapRunner::run_presets` / `run_spawn`
  (`project_bootstrap.h`) — delegates the entire first-project scaffold/config
  boundary to the TUI headless surface.
- `AgentSetupStore::load` / `save` (`agent_setup_store.h`) — one bounded
  existing-Agent setup snapshot/draft and one staged transaction over the
  selected `init.json`, `.agent.json`, exact configured env leaf, peer
  `manifest.preset` blocks when a replacement policy was supplied, and TUI-
  parity peer orchestrator fields when the selected Agent is an orchestrator.
  It does not scaffold or add an Agent.

`NativeShell` routes `/setup` for the exact current selection through that
existing-Agent store, Desktop's full saved/template catalog reader, and the
shared in-window setup wizard/editor. It retains the loaded state until Save
or cancellation, never invokes TUI preset discovery or spawn for this route,
and refreshes the selected project only after `saved` or `no_change`. New
Project remains the separate `ProjectBootstrapRunner` `presets`/`spawn` flow.

State models: `WorkspaceSelectionState` (`workspace_selection.h`) is the only
owner of the accepted active project and the selected Agent directory key. A
caller proposes typed transitions only; the model performs no reads.
`DirectMailboxSnapshotIndex` (`direct_conversation_history.h`) is the only
owner of mailbox single-flight/generation acceptance. It performs no reads or
threading; `NativeShell` supplies fingerprints/results and runs accepted jobs.
The worker proves unchanged histories and exact append prefixes structurally,
and the index assigns stable accepted per-history revisions.
`NativeShell` owns the session-only pending-publication projection layered on
that accepted history. It keys published rows by canonical project and Agent,
retires them only when an accepted snapshot contains the same ID, and derives
one presentation revision/append lineage from the merged history.
A render may borrow
the current immutable snapshot only for its synchronous call stack and never
retain that pointer across snapshot acceptance.
`KanbanSnapshotIndex` is the corresponding Kanban source/index owner;
`NativeShell` alone owns its low-priority worker, generation acceptance,
coalescing, and stale-while-revalidate presentation.

## Adapters

- `ProjectAttachment` (`project_attachment.h`) is the containment adapter: it
  accepts an existing directory, canonicalizes it, and resolves contained
  relative paths. It is Qt-independent and used by every reader/owner that
  walks the project tree.
- Attachment selection (`attachment_selection.h`) is a separate Qt-independent
  local-file adapter. It canonicalizes and opens selected sources, measures the
  opened regular files, retains their device/inode identities, deduplicates
  those identities, owns the shared pure filename classifier for the small
  explicit image-extension set, and
  applies the 25 MiB per-file and 100 MiB cumulative limits in caller order. A
  publisher must revalidate later because these facts do not close the
  time-of-check/time-of-use gap.
- `posix_internal` (`posix_descriptor_primitives.h`) is the shared filesystem
  adapter seam: descriptor ownership, no-follow one-leaf opens, and `safe_leaf`
  validation. It is internal, links nothing, and carries no domain policy.
- The pinned toolkit (`desktop-app::lib_ui`) is consumed only by the shell
  library and the smoke executable; readers never link it. Application
  composition owns its process-global integrations, animations manager,
  palette/styles, and one `QApplication`-owned emoji runtime shared by every
  shell; composer widgets consume those prerequisites but do not own them.
- The TUI subprocess (`lingtai-tui`) is the outbound adapter for explicit
  first-project bootstrap, reached only through `ProjectBootstrapRunner` with
  exact separate argv and a bounded JSON parse.
- The TUI executable resolver is a separate pre-launch adapter. It returns the
  supplied candidate path (including an accepted symlink path) only when it
  resolves to an executable regular file; malformed receipts fail closed.

## Contract rules

1. **One source of ownership per behavior.** Roster truth lives only in
   `WorkspaceSelectionState` + the sole `agents_` snapshot; conversation rows
   only in the `direct_conversation_history` shared projection/index;
   Presets only in `read_agent_preset_summary`. No second owner exists.
2. **Readers read.** Every project-tree reader opens its source one no-follow
   leaf at a time through `posix_internal`, bounds the actual read, and never
   writes. Attachment selection instead canonicalizes an arbitrary caller-
   selected local path and verifies the opened descriptor; it is not a
   project-tree walk. A missing/unreadable/unsafe source reduces to a coarse
   state; one bad sibling never hides a healthy neighbor. Conversation
   attachment paths discard serialized parents, root the validated basename
   under the current entry, and remain observations rather than later action
   authorization. Open/Reveal therefore refresh and reopen at action time;
   missing, replaced, linked, non-regular, escaping, or identity-mismatched
   files fail closed.
3. **Publishers/launchers own side effects.** Only `send_direct_mail`,
   `request_agent_sleep`, `start_agent`, `AgentSetupStore` (composed by the
   shell's selected-Agent `/setup` route), and
   `ProjectBootstrapRunner` write or launch. Direct mail `queued` means only
   that the complete human outbox leaf
   was published; none claims kernel pickup, target acceptance, delivery,
   liveness, or lifecycle from the local write/start alone.
4. **The shell composes.** `NativeShell` composes the widgets, native dialogs, and
   timer, re-derives visible routes from C1 truth, and proposes transitions
   through the model. Its window factory also establishes the pinned toolkit's
   application-lifetime prerequisites before `AgentDetailView` can construct
   the composer. It also owns the composer-local dispatch after calling
   `parse_slash_command` on raw text: every parsed command terminates locally
   before `send_direct_mail`. U2 lifecycle slash dispatch may invoke only the
   existing `request_agent_sleep` and `start_agent` owners through their
   existing selected-Agent handlers; `NativeShell` does not reimplement their
   lifecycle behavior, and this composition changes no public interface.
5. **UI widgets do not absorb domain/business behavior.** `AgentRoster`
   renders rows and reports clicks; `ConversationSurface` renders rows and
   paints bubbles; `AgentDetailView` owns only the composer draft,
   presentation, thumbnail attempts, and transient/persistent feedback.
   NativeShell still owns picker orchestration, route resolution, publication,
   and the injectable external Open/Reveal action. These widgets hold only
   view state and emit the exact presentation-time attachment request.
6. **The route stays pure.** `resolve_direct_conversation_route` performs no
   filesystem access; its library links only `lingtai_desktop_core`, making a
   second discovery read structurally impossible.
7. **Containment is always enforced.** Every project-tree walk anchors at the
   canonical root and refuses intermediate symlinks; `ProjectAttachment::resolve`
   is used where a full path is needed (e.g. `start_agent`).
8. **No unowned scaffold/config/registry writes.** Desktop never scaffolds an
   Agent or broadly regenerates its configuration; explicit first-project
   bootstrap delegates that entire boundary to the TUI headless surface.
   `AgentSetupStore` is the sole exception for an existing Agent: it preserves
   the full documents and patches only the fields declared by
   `AgentSetupDraft`, the one soul-flow key in the exact configured env leaf,
   peer preset policy only for a non-empty replacement list, and orchestrator
   `llm`/`capabilities`/`soul`/`context_limit`/`env_file` propagation while
   forcing peer karma/nirvana false and deleting peer addons. Peer MCP,
   identity/runtime, unknown fields, and safe admin extras remain untouched.
   Every target participates in one staged transaction with rollback.
   Agent-local side effects (the one outbox leaf, the one `.sleep` marker, and
   the Start log plus launch) are the owners' own bounded behavior, not
   project/Agent scaffold or config writes.

## Contract tests

Each owned library target has one focused CMake/ctest contract; the fixture
paths and names are in [`../ANATOMY.md`](../ANATOMY.md) and `CMakeLists.txt`:

- `tests/agent_projection_test.cpp` — `agent_projection`.
- `tests/direct_conversation_route_test.cpp` — `direct_conversation_route`.
- `tests/direct_conversation_history_test.cpp` — `direct_conversation_history`.
- `tests/direct_conversation_attachment_actions_test.cpp` —
  `direct_conversation_attachment_actions`.
- `tests/direct_mail_publisher_test.cpp` — `direct_mail_publisher`.
- `tests/agent_preset_summary_test.cpp` — `agent_preset_summary`.
- `tests/agent_setup_store_test.cpp` — `agent_setup_store`.
- `tests/preset_catalog_test.cpp` — `preset_catalog`.
- `tests/agent_sleep_test.cpp` — `agent_sleep`.
- `tests/kanban_model_test.cpp` — `kanban_model`.
- `tests/posix_descriptor_primitives_test.cpp` — `posix_descriptor_primitives`.
- `tests/workspace_selection_test.cpp` — `workspace_selection`.
- `tests/project_attachment_test.cpp` — `project_attachment`.
- `tests/attachment_selection_test.cpp` — `attachment_selection`.
- `tests/native_shell_test.cpp` — `native_shell_behavior` (links the shell +
  `lib_ui` + `crl_integration.cpp`).
- `tests/test_native_shell.py` — `native_shell` (process persistence and
  smoke-order, via the smoke executable).

Behavior-level anchors, not a second contract suite, live in
[`src/BEHAVIORS.md`](BEHAVIORS.md).

## Maintenance

- Keep the ownership rules and the CMake link edges aligned: a reader that
  gains a write, a widget that absorbs policy, or a second owner for a source
  is a contract violation even if tests still pass.
- Update this contract and the paired `src/ANATOMY.md` in the same change as
  the code when a boundary, an ownership, or a behavioral promise changes;
  update the root `../ANATOMY.md` only when the top-level inventory changes.
- The contract is normative: if implementation and this contract disagree,
  treat the implementation as defective unless an authorized change updates
  both together.
