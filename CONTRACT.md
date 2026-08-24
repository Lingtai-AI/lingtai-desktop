# LingTai Desktop Root Contract

This contract is the stable cross-folder and public-ownership boundary of the
LingTai Desktop repository. It is normative when implementation behavior
disagrees. It defines who owns what, what may depend on what, and what is
forbidden. It does not restate per-owner behavior (that lives in each owner's
own `ANATOMY.md`/`CONTRACT.md`); it states the repository-level graph.

## Ownership

- `src/` owns the Qt adaptation surface and every leaf seam named in the root
  `ANATOMY.md`. Each seam is one library with one public function/class
  (declared `CMakeLists.txt:211-308`).
- `src/ui/` owns the two LingTai widgets: `AgentRoster` and
  `ConversationSurface`. They are owned product UI, not Telegram screens.
- `tests/` owns the repository's observable-behavior contracts and the two
  Python gates.
- `cmake/desktop-app-toolkit-lock.json` is the exact, authoritative source of
  dependency provenance. Changing a pin is a reviewed, coherent lock update
  validated by `tests/test_repository_contract.py`.
- `scripts/` owns the bootstrap/configure/build/smoke entry points;
  `scripts/bootstrap-deps.sh` is the only sanctioned way to populate `.deps/`.
- `main.cpp` owns application composition: the one fallback interpreter, the
  one configured TUI executable, and the `--smoke` path. The shell performs no
  project, registry, or settings writes beyond the explicit user-triggered
  actions (composer send, Request sleep, Start Agent, plus the explicit New
  Project bootstrap delegation).

## Public (cross-folder) interfaces

These are the only stable cross-folder seams; everything else is private to
its folder.

- `lingtai_desktop_core`: `attach_project`, `ProjectAttachment::resolve`,
  `WorkspaceSelectionState`. Qt-independent; no Telegram dependency.
- `lingtai_desktop_attachment_selection`: `preflight_attachments`, the
  Qt-independent, read-only local selection fact model. It does not authorize
  publication; a later publisher must revalidate accepted sources.
- `lingtai_desktop_posix_primitives`: the descriptor-relative, no-follow walk
  primitives. Internal seam consumed privately by the readers/publisher; no
  consumer inherits it transitively.
- The read, route, and action seams under `src/`, each with the single
  free function/class in its header:
  `project_agents`, `resolve_direct_conversation_route`,
  `read_direct_conversation`, `send_direct_mail`,
  `read_agent_preset_summary`, `request_agent_sleep`
  (+ its two observation functions), `start_agent`, `ProjectBootstrapRunner`.
- `NativeShell` is the one composition root C5 owns; it proposes transitions
  only through `WorkspaceSelectionState`.
- `LINGTAI_LIB_UI_FULL_TARGET_SMOKE_OK` and `LINGTAI_NATIVE_SHELL_READY` are
  the only process-observable markers, emitted only on the `--smoke` path.

## Kernel boundary

- Desktop has **no link edge** into the `lingtai` kernel package (no Python
  import, no protocol, no account). Its only interface is on-disk artifacts
  under an accepted `.lingtai` project plus two subprocess surfaces.
- Producer-side artifact contracts live in the kernel repo and are normative
  for shape: the append-only `logs/events.jsonl` journal
  (`src/lingtai/kernel/event_journal/CONTRACT.md`), the mailbox envelope,
  `system/manifest.resolved.json`, and the `.sleep`
  marker. Desktop reads these read-only; it writes only its own outbox leaf,
  the `.sleep` marker, and the started Agent's `logs/` directory.
- The TUI executable (`lingtai-tui`) is invoked only through `NativeShell`'s
  exact separate-argv `presets`/`spawn` calls and `ProjectBootstrapRunner`;
  never a shell string, never a joined command line.

## Forbidden dependencies

- **No Telegram product code**: no account, protocol, chat, message, media,
  contact, cache, or high-level UI code may be imported into `src/` or
  `tests/`. Telegram is a visual/mature-interaction oracle only — never an
  owner, producer, or dependency.
- **No `desktop-app::lib_ui` fork or patch**: the pinned full target is built
  unmodified from the locked sources (`CMakeLists.txt:165-167`).
- **No writes outside the declared leaves**; read seams are `noexcept` and
  write nothing.
- **No committed build inputs**: `.deps/`, `build/`, Qt SDK trees, and binary
  icon copies must never be tracked.
- **No additional first-project machinery**: Desktop never manually writes
  project/Agent configuration, never adds an Agent to an existing network, and
  delegates first-project bootstrap to the TUI headless surface.

## Cross-doc graph

- This root contract pairs with the root `ANATOMY.md`; both list each other.
- Child doc sets for `src/`, `src/ui/`, and `tests/` are the expected owners
  of the per-folder navigation and interface detail; they list this root set
  and the root set routes into them.
- TUI is the functional oracle: exact-argv subprocess and envelope semantics
  are validated against the TUI surface. Telegram is the visual oracle only:
  layout/mature-interaction fidelity is compared, never copied as code.
