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
   (`main.cpp`) is the only place that chooses the concrete fallback
   interpreter and the configured TUI executable, and it passes them in
   through the two setters. Do not hard-code either in the shell or any
   reader.
5. Report a mismatch between this contract and the code rather than silently
   rewriting the promise to match accidental behavior.

## Port

The boundary an external caller (mainly `NativeShell` and `main.cpp`) uses is
a small set of free functions and two small value objects. Every read
function is `noexcept`, stateless, and returns one snapshot; every
side-effecting function returns one coarse result enum.

Reads (no writes, no durable state):

- `project_agents(attachment)` → `AgentSnapshot` (`agent_projection.h`).
  The snapshot is shared live truth and keeps the human pseudo-agent row
  (routing, mailbox, and selected-detail truth consume it); only the
  `AgentRoster` presentation omits that row.
- `resolve_direct_conversation_route(attachment, snapshot, key)` →
  `std::optional<DirectConversationRoute>` (`direct_conversation_route.h`).
- `read_direct_conversation(route)` → `DirectConversationHistory`
  (`direct_conversation_history.h`).
- `read_agent_preset_summary(attachment, key)` → `AgentPresetSummary`
  (`agent_preset_summary.h`).

Side-effecting operations (the only writers/launchers; each owns one explicit
bounded side-effect scope):

- `send_direct_mail(route, text)` → `DirectMailSendResult`
  (`direct_mail_publisher.h`) — one human `outbox/<id>` leaf.
- `request_agent_sleep(attachment, key)` → `AgentSleepRequestResult`,
  plus `capture_agent_sleep_event_baseline` / `observe_agent_sleep_received`
  (`agent_sleep.h`) — one `.sleep` marker.
- `start_agent(attachment, key, fallback_python)` → `AgentLaunchResult`
  (`agent_launch.h`) — owns the Agent's `logs/agent.log` plus the detached
  launch.
- `ProjectBootstrapRunner::run_presets` / `run_spawn`
  (`project_bootstrap.h`) — delegates the entire project scaffold/config
  boundary to the TUI headless surface.

State model (`workspace_selection.h`): `WorkspaceSelectionState` is the only
owner of the accepted active project and the selected Agent directory key. A
caller proposes typed transitions only; the model performs no reads.

## Adapters

- `ProjectAttachment` (`project_attachment.h`) is the containment adapter: it
  accepts an existing directory, canonicalizes it, and resolves contained
  relative paths. It is Qt-independent and used by every reader/owner that
  walks the project tree.
- `posix_internal` (`posix_descriptor_primitives.h`) is the shared filesystem
  adapter seam: descriptor ownership, no-follow one-leaf opens, and `safe_leaf`
  validation. It is internal, links nothing, and carries no domain policy.
- The pinned toolkit (`desktop-app::lib_ui`) is consumed only by the shell
  library and the smoke executable; readers never link it.
- The TUI subprocess (`lingtai-tui`) is the outbound adapter for explicit
  first-project bootstrap, reached only through `ProjectBootstrapRunner` with
  exact separate argv and a bounded JSON parse.

## Contract rules

1. **One source of ownership per behavior.** Roster truth lives only in
   `WorkspaceSelectionState` + the sole `agents_` snapshot; conversation rows
   only in `read_direct_conversation`;
   Presets only in `read_agent_preset_summary`. No second owner exists.
2. **Readers read.** Every reader opens its source one no-follow leaf at a
   time through `posix_internal`, bounds the actual read, and never writes.
   A missing/unreadable/unsafe source reduces to a coarse state; one bad
   sibling never hides a healthy neighbor.
3. **Publishers/launchers own side effects.** Only `send_direct_mail`,
   `request_agent_sleep`, `start_agent`, and `ProjectBootstrapRunner` write or
   launch. They never claim target acceptance, queueing, liveness, or
   lifecycle from the local write/start alone.
4. **The shell composes.** `NativeShell` composes the widgets, dialog, and
   timer, re-derives visible routes from C1 truth, and proposes transitions
   through the model. It does not re-implement a reader.
5. **UI widgets do not absorb domain/business behavior.** `AgentRoster`
   renders rows and reports clicks; `ConversationSurface` renders rows and
   paints bubbles. Both hold only view state.
6. **The route stays pure.** `resolve_direct_conversation_route` performs no
   filesystem access; its library links only `lingtai_desktop_core`, making a
   second discovery read structurally impossible.
7. **Containment is always enforced.** Every project-tree walk anchors at the
   canonical root and refuses intermediate symlinks; `ProjectAttachment::resolve`
   is used where a full path is needed (e.g. `start_agent`).
8. **No scaffold/config/registry writes.** Desktop never scaffolds project or
   Agent directories or writes their configuration; the explicit first-project
   bootstrap delegates that entire boundary to the TUI headless surface.
   Agent-local side effects (the one outbox leaf, the one `.sleep` marker, and
   the Start log plus launch) are the owners' own bounded behavior, not
   project/Agent scaffold or config writes.

## Contract tests

Each owned library target has one focused CMake/ctest contract; the fixture
paths and names are in [`../ANATOMY.md`](../ANATOMY.md) and `CMakeLists.txt`:

- `tests/agent_projection_test.cpp` — `agent_projection`.
- `tests/direct_conversation_route_test.cpp` — `direct_conversation_route`.
- `tests/direct_conversation_history_test.cpp` — `direct_conversation_history`.
- `tests/direct_mail_publisher_test.cpp` — `direct_mail_publisher`.
- `tests/agent_preset_summary_test.cpp` — `agent_preset_summary`.
- `tests/agent_sleep_test.cpp` — `agent_sleep`.
- `tests/posix_descriptor_primitives_test.cpp` — `posix_descriptor_primitives`.
- `tests/workspace_selection_test.cpp` — `workspace_selection`.
- `tests/project_attachment_test.cpp` — `project_attachment`.
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
