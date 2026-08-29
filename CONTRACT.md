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
- `tests/` owns the repository's observable-behavior contracts and the Python
  repository, packaging, and process-smoke gates.
- `cmake/desktop-app-toolkit-lock.json` is the exact, authoritative source of
  dependency provenance. Changing a pin is a reviewed, coherent lock update
  validated by `tests/test_repository_contract.py`.
- `scripts/` owns the bootstrap/configure/build/smoke and macOS package/verify
  entry points;
  `scripts/bootstrap-deps.sh` is the only sanctioned way to populate `.deps/`.
- `scripts/app_archive.py` owns the primary portable-App archive production
  boundary. It never mutates the input App and exclusively publishes a complete
  archive/manifest pair without replacing a concurrent target.
  `scripts/verify-app-archive.py` independently owns incremental untrusted-member
  preflight before extraction, a 512 MiB compressed-archive ceiling, private
  extraction, exact archive/App-tree binding, and universal executable
  verification. The older DMG producer/verifier remains an optional release
  experiment and is not an installer input.
- `scripts/desktop_user_cli.py` solely owns the unprivileged, HOME-derived
  official-release discovery/download, cached update offer, and
  install/update/current/receipt/doctor/launch/uninstall transaction.
  `scripts/install-macos-app.py` is a thin bootstrap and duplicates no policy.
  Remote acquisition is fixed to stable `Lingtai-AI/lingtai-desktop` GitHub
  Releases behind an injected HTTPS transport, and stops at a private temporary
  archive/manifest pair consumed by the existing authoritative `install()` seam.
  Each URL hop must already be ASCII (ordinary percent-encoded paths remain
  accepted), and every numeric version component is bounded to nine ASCII
  decimal digits before integer, path, or cache use. Explicit local pairs remain
  supported. Normal commands validate syntax before update side effects and use
  the owned bounded, single-link `update-check.json`: noninteractive calls notice
  only, while interactive calls
  require an explicit default-No `y`/`yes` before the same verified transaction.
  Explicit `update` forces fresh discovery and never prompts, but malformed or
  substituted owned cache state remains a fail-closed integrity precondition. The
  command never mutates PATH/profiles and never weakens quarantine or Gatekeeper.
  Existing shared `.local` parent modes are outside its ownership. Managed
  publication becomes visible only after mode preparation, rollback removes
  only invocation-created inode identities, and uninstall rejects the entire
  transaction before deletion if any ancestor, root child, receipt, version,
  bundle digest, CLI file, verifier, or launcher is unknown or substituted.
- `ShellHost` owns application composition and the one fallback kernel
  interpreter; `main.cpp` owns the `--smoke` path. The shell performs no
  project, registry, or settings writes beyond the explicit user-triggered
  actions (composer send, Request sleep, Start Agent, plus the explicit New
  Project creation transaction).

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
  `read_direct_conversation`, `revalidate_direct_conversation_attachment`,
  `send_direct_mail`,
  `read_agent_preset_summary`, `load_preset_catalog`, `write_agent_signal`,
  `request_agent_sleep` (+ its two observation functions), `launch_agent`,
  `AgentLifecycleController`, `AgentSetupStore`,
  `create_project`, `ProjectCreationRunner`.
- `NativeShell` is the one composition root C5 owns; it proposes transitions
  only through `WorkspaceSelectionState`. It also owns the injectable
  attachment Open/Reveal external-action seam; the conversation widget only
  emits presentation identity and never launches an application.
- `LINGTAI_LIB_UI_FULL_TARGET_SMOKE_OK` and `LINGTAI_NATIVE_SHELL_READY` are
  the only process-observable markers, emitted only on the `--smoke` path.

## Kernel boundary

- Desktop has **no link edge** into the `lingtai` kernel package (no Python
  import, no protocol, no account). Its interface is on-disk artifacts under
  an accepted `.lingtai` project, the exact configured env leaf named by a
  selected Agent's bounded `init.json`, advisory lock/process observation,
  and the exact detached kernel-runtime launch.
- Producer-side artifact contracts live in the kernel repo and are normative
  for shape: the append-only `logs/events.jsonl` journal
  (`src/lingtai/kernel/event_journal/CONTRACT.md`), the mailbox envelope,
  `system/manifest.resolved.json`, lifecycle markers, heartbeat, lock, and
  context-completion artifacts.
  marker. Outside an explicit `AgentSetupStore` save, Desktop reads Agent
  configuration read-only except the lifecycle controller's atomic
  `manifest.preset.active` update during an authorized hard refresh. Its other
  writers remain limited to its own outbox leaf, fixed lifecycle markers, and
  the started Agent's `logs/` directory.
- New Project and existing-Agent `/setup` both discover presets through
  Desktop's bounded `load_preset_catalog`. `create_project` owns the initial
  fail-closed filesystem transaction and reports its stable typed stage plus
  safe detail. Runtime readiness is not a publication precondition:
  `AgentLifecycleController` owns the post-commit first launch plus all later
  `/sleep`, `/suspend`, `/cpr`, `/clear`, and `/refresh` kernel
  filesystem/process contracts. No production path executes or discovers a
  TUI binary.

## Forbidden dependencies

- **No Telegram product code**: no account, protocol, chat, message, media,
  contact, cache, or high-level UI code may be imported into `src/` or
  `tests/`. Telegram is a visual/mature-interaction oracle only — never an
  owner, producer, or dependency.
- **No `desktop-app::lib_ui` fork or patch**: the pinned full target is built
  unmodified from the locked sources (`CMakeLists.txt:165-167`).
- **No writes outside the declared leaves**; the narrowly declared setup
  transaction may patch only its owned existing-Agent configuration leaves,
  including the exact descriptor-walked env leaf already named by the selected
  Agent's bounded `init.json`, and read seams are `noexcept` and write nothing.
- **No committed build inputs**: `.deps/`, `build/`, Qt SDK trees, and binary
  icon copies must never be tracked.
- **One primary public release contract**: the terminal-installed
  `lingtai-desktop` command downloads exactly one versioned universal
  `LingTai.app` archive and its exact JSON manifest from the fixed official
  stable GitHub Release, then independently and fail-closed verifies the pair
  and App before atomically publishing it under the user-level managed root.
  Developer ID signing, hardened runtime, timestamping, notarization, stapling,
  a DMG, `/Applications`, sudo, and quarantine bypass are not prerequisites for
  this primary archive publication route. `packaging_git_*` manifest facts
  identify only the clean tracked packaging checkout and must never be
  represented as input-App build provenance.
- **No DMG-bound managed lifecycle**: the primary terminal install/update path
  consumes a portable App archive and never mounts or detaches a disk image.
  The optional DMG experiment may enforce its own signing and notarization
  gates, but it neither defines nor gates primary archive publication.
  Archive packaging Git facts describe the packaging checkout only; they never
  claim to identify the App build.
- **No privileged or unscoped remote install path**: the managed lifecycle refuses
  effective uid 0, owns only `$HOME/.local/bin/lingtai-desktop` and
  `$HOME/.local/share/lingtai-desktop`, and has no sudo, system prefix,
  non-official release source, silent update, downgrade bypass, or destructive
  no-argument uninstall.
- **One bounded first-project owner**: `AgentSetupStore` updates only
  setup-owned fields of existing Agents; `create_project` alone may publish the
  minimum initial tree and creates no global registry or TUI recipe state.

## Cross-doc graph

- This root contract pairs with the root `ANATOMY.md`; both list each other.
- Child doc sets for `src/`, `src/ui/`, and `tests/` are the expected owners
  of the per-folder navigation and interface detail; they list this root set
  and the root set routes into them.
- TUI is a functional compatibility oracle for shared on-disk semantics, never
  a runtime process dependency. Telegram is the visual oracle only:
  layout/mature-interaction fidelity is compared, never copied as code.
