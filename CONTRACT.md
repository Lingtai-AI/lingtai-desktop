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
  Explicit `update` forces fresh discovery and never prompts. The command never
  mutates PATH/profiles and never weakens quarantine or Gatekeeper.
  Existing shared `.local` parent modes are outside its ownership. Managed
  publication becomes visible only after mode preparation, rollback removes
  only invocation-created inode identities, and uninstall rejects the entire
  transaction before deletion if any ancestor, root child, receipt, version,
  bundle digest, CLI file, verifier, or launcher is unknown or substituted.
- `ShellHost` owns application composition: the one fallback interpreter and
  the one configured TUI executable; `main.cpp` owns the `--smoke` path. The shell performs no
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
  `read_direct_conversation`, `revalidate_direct_conversation_attachment`,
  `send_direct_mail`,
  `read_agent_preset_summary`, `load_preset_catalog`, `write_agent_signal`,
  `request_agent_sleep` (+ its two observation functions), `launch_agent`,
  `AgentLifecycleController`, `AgentSetupStore`,
  `ProjectBootstrapRunner`.
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
- The TUI executable (`lingtai-tui`) is invoked only for New Project through
  `ProjectBootstrapRunner`'s exact separate-argv `presets`/`spawn` calls;
  existing-Agent `/setup` uses Desktop's read-only `load_preset_catalog` and
  never invokes either command. No TUI call uses a shell string or joined
  command line. `/sleep`, `/suspend`, `/cpr`, `/clear`, and `/refresh` never
  invoke or discover the TUI; `AgentLifecycleController` implements those
  kernel filesystem/process contracts directly.
- `resolve_tui_executable` is the sole default-executable discovery owner. It
  consumes only injected PATH/home/system roots, validates regular executable
  candidates, and reads only the bounded managed-install receipt.

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
- **No ambiguous release state**: ad-hoc/unsigned diagnostic output is never
  release-ready. A public package requires Developer ID Application signing,
  hardened runtime, timestamp, Apple acceptance, stapling, and strict package
  verification; any absent prerequisite or unsuccessful check is fatal.
  `packaging_git_*` manifest facts identify only the clean tracked packaging
  checkout and must never be represented as input-App build provenance.
- **No DMG-bound managed lifecycle**: the primary terminal install/update path
  consumes a portable App archive and never mounts or detaches a disk image.
  Archive packaging Git facts describe the packaging checkout only; they never
  claim to identify the App build.
- **No privileged or unscoped remote install path**: the managed lifecycle refuses
  effective uid 0, owns only `$HOME/.local/bin/lingtai-desktop` and
  `$HOME/.local/share/lingtai-desktop`, and has no sudo, system prefix,
  non-official release source, silent update, downgrade bypass, or destructive
  no-argument uninstall.
- **No additional first-project machinery**: `AgentSetupStore` updates only
  setup-owned fields of existing Agents and never scaffolds or adds an Agent;
  first-project bootstrap remains delegated to the TUI headless surface.

## Cross-doc graph

- This root contract pairs with the root `ANATOMY.md`; both list each other.
- Child doc sets for `src/`, `src/ui/`, and `tests/` are the expected owners
  of the per-folder navigation and interface detail; they list this root set
  and the root set routes into them.
- TUI is the functional oracle: exact-argv subprocess and envelope semantics
  are validated against the TUI surface. Telegram is the visual oracle only:
  layout/mature-interaction fidelity is compared, never copied as code.
