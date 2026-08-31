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
- `scripts/desktop_user_cli.py` solely owns the unprivileged, HOME-derived App
  official-release discovery/download, cached update offer, and
  install/update/current/receipt/doctor/launch/uninstall transaction. It also
  owns the exact `lingtai.desktop.support/v1` manifest/state/pending models,
  including the exact numbered `release_tag == "v" + support_version` identity,
  immutable local and official support-generation staging, support diagnostics,
  and the no-write/no-network `support_self_test`. Official support acquisition
  accepts only the same stable `Lingtai-AI/lingtai-desktop` release, exact
  `support-manifest.json` plus both exact payload assets, bounded official HTTPS
  routes, and canonical manifest-declared size/SHA-256 facts; it publishes no
  managed byte until all three private downloads compile and revalidate
  (`scripts/desktop_user_cli.py:648-848`). `scripts/support_release.py` produces
  and independently validates that deterministic exact three-file asset set
  without clobber (`scripts/support_release.py:72-144`).
  `scripts/support_bootstrap.py` is the stable, digest-bound installed
  launcher: before importing support code it validates the real managed chain,
  exact pointer/generation/manifest/payload set, carries the exact validated bytes
  through import/self-test, handles one pending switch only for the exact canonical
  full invocation argv that staged it, and commits or rolls back only after
  identity/hash and App/support-plane revalidation. Staging replaces the supplied
  full argv's first element with the absolute installed launcher before hashing,
  pending publication, and exec, while preserving every following argument and
  never reinterpreting argv0 as an omitted command argument. The stable wrapper
  consumes the value-only reexec marker before candidate import and injects only
  its derived one-shot boolean after import, so the resumed command cannot loop
  or stage support twice (`scripts/support_bootstrap.py:1299-1354`). Candidate code runs
  in an isolated, minimal-environment child with private HOME/cwd/TMPDIR, bounded
  stdio/time, and a pre-candidate audit boundary denying writes, network, process/
  exec, native-loader escape, frame escape, and early exit. A denied capability,
  including candidate use of public `os.write`/`posix.write`, records a sticky
  marker in a production-parent-owned pipe before candidate handling resumes.
  The inherited audit descriptor is removed from candidate argv, and the genuine
  terminal write remains local to the active wrapper call rather than a mutable
  candidate-visible global. The parent accepts only that wrapper-owned singleton
  clean marker plus an exact true result and zero wait status. Rollback is
  failed-state-first,
  authenticates a retained source-pointer identity across every durable recovery
  boundary, and can abort an uncommitted mutated target without trusting its bytes.
  It never discovers releases or mutates the App plane. `scripts/install-macos-app.py`
  is a thin initial bootstrap and duplicates no lifecycle policy.
  Remote acquisition for both planes is fixed to stable
  `Lingtai-AI/lingtai-desktop` GitHub Releases behind an injected HTTPS
  transport. App acquisition stops at a private archive/manifest pair consumed
  by `install()`; support acquisition stops at a separately bounded private
  manifest/two-payload set consumed by the same immutable generation/pending
  transaction as local support staging. V1 trust is exactly TLS plus the official
  GitHub origin/route and declared hashes; it is not release signing.
  Each URL hop must already be ASCII (ordinary percent-encoded paths remain
  accepted), and every numeric version component is bounded to nine ASCII
  decimal digits before integer, path, or cache use. Explicit local pairs remain
  supported. Normal commands validate syntax before update side effects and use
  the owned bounded, single-link `update-check.json`: noninteractive calls notice
  only, while interactive calls
  require an explicit default-No `y`/`yes` before the same verified transaction.
  Explicit `update` forces fresh discovery and never prompts, but malformed or
  substituted owned App cache state remains a fail-closed integrity precondition.
  Independently, `support/update-check.json` records the exact support
  version/tag/generation/manifest digest/time/decline decision under a bounded
  canonical schema and restoration-capable atomic replacement. Publication
  validates a provisional stage but success linearizes only at the final atomic
  exchange of a second independent staged inode into canonical. Any racer that
  wins before that point is atomically displaced and never accepted as staged
  success: an existing cache restores its exact retained prior and preserves the
  racer under a distinct failure-only
  `.preserved-support-update-cache-racer-*` leaf; an initially absent cache
  atomically returns the racer to canonical. Restoration has its own bounded
  linearization path: exchange when canonical exists, exclusive hard link when
  it disappeared, retrying boundedly if name state changes between observation
  and operation. Clean publication retains no transaction leaf, ends with one
  mode-0600 link, and later cleanup removes only recorded identities. No
  snapshot-at-Python-return guarantee is made against mutation after an atomic
  success/restoration point (`scripts/desktop_user_cli.py:1984-2262`). Ordinary
  valid commands check it on
  its own cadence: corruption or provider failure warns and continues without
  replacing prior bytes, non-TTY only notices, and TTY defaults No and stages
  only stripped `y`/`yes`. Explicit official update attempts support first with
  exact one-shot retry, then App; a visible support failure may leave support
  unchanged and continue the independent App transaction
  (`scripts/desktop_user_cli.py:3274-3648`). Local App pairs touch no transport or
  either cache.
  The staged App runs the exact `--smoke` invocation under its isolated
  environment with a 60-second ceiling and must emit the ready then full-target
  markers; timeout, nonzero exit, or absent/out-of-order markers aborts before
  publication. The command never mutates PATH/profiles and never weakens
  quarantine or Gatekeeper.
  Existing shared `.local` parent modes are outside its ownership. Every App
  receipt authority read enforces current-user ownership, regular single-link
  type, mode 0600, bounded size, exact schema, and canonical bytes. An App update
  atomically exchanges `current` while retaining the exact observed prior symlink;
  a raced pointer is restored and rejected, and any later failure restores that
  exact inode before new receipt/version cleanup. A fresh install journals the
  exact App/support identities before publication and resumes only
  that bounded canonical transaction after process death; ordinary failure removes
  only exact still-owned invocation-created artifacts and empty directories.
  Managed publication becomes visible only after mode preparation, and App/support
  transactions remain independent even when the public `update` command sequences
  both: each App transaction leaves `support/current`, support state, generations,
  cache, and launcher unchanged, while each support transaction leaves App
  versions, receipts, bundle bytes/inodes, cache, and App `current` unchanged.
  Support state binds last-good/high-water/failed generation hashes and permits a
  lower local last-good only with the exact failed high-water record. Public
  explicit retry is journaled and consumed once. `uninstall --version` is App-only
  and neither requires nor inspects any support path. `uninstall --all` rejects the complete operation before deletion if
  any ancestor/root child, App receipt/version/bundle, support generation/file,
  support pointer/state/pending/cache, or stable launcher is unknown, linked,
  mode-mismatched, invalid, or substituted.
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
  fail-closed filesystem transaction, including the localized kernel-consumed
  first-boot `.prompt` and either byte-exact reviewed Comment text or the
  localized adaptive `comment.md` playbook. It reports its stable typed stage
  plus safe detail. Runtime readiness is not a publication precondition:
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
  minimum initial tree. Its per-Agent `.prompt` and `comment.md` are
  kernel-consumed project behavior, not TUI recipe state. It creates no global
  registry, `.tui-asset`, project `.recipe`, credential draft, or TUI recipe
  reconciliation/snapshot state.

## Cross-doc graph

- This root contract pairs with the root `ANATOMY.md`; both list each other.
- Child doc sets for `src/`, `src/ui/`, and `tests/` are the expected owners
  of the per-folder navigation and interface detail; they list this root set
  and the root set routes into them.
- TUI is a functional compatibility oracle for shared on-disk semantics, never
  a runtime process dependency. Telegram is the visual oracle only:
  layout/mature-interaction fidelity is compared, never copied as code.
