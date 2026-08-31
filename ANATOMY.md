# LingTai Desktop Repository Anatomy

LingTai Desktop is the Qt 6.11.1 native-desktop adaptation of the LingTai
product. It is **not** a Telegram Desktop fork and imports no Telegram product
code: it builds the complete pinned `desktop-app::lib_ui` toolkit target from
source and adapts Qt's widget stack to LingTai's on-disk project model. This
file is the root navigation map; it routes into the real owners below and into
the child docs that map them (`src/`, `src/ui/`, `tests/`). Code is the
structural source of truth; this map must not duplicate the child maps.

## Top-level layout

```
CMakeLists.txt                         top-level source build graph (targets, ctest, Qt)
cmake/desktop-app-toolkit-lock.json    exact toolkit/third-party/Qt/resource lock
scripts/bootstrap-deps.sh              verified local source/resource bootstrap
scripts/configure.sh                   Qt-aware CMake configure wrapper
scripts/build.sh                       target build wrapper
scripts/smoke.py                       bounded offscreen native-shell smoke runner
scripts/app_archive.py                 primary portable-App archive producer
scripts/package-app-archive.py         portable-App packaging CLI
scripts/verify-app-archive.py          independent safe archive verifier/extractor
scripts/macos_packaging.py             testable fail-closed macOS package owner
scripts/package-macos.py               optional diagnostic/release DMG CLI
scripts/verify-macos-package.py        optional independent DMG verifier
scripts/desktop_user_cli.py            App lifecycle + official/local managed support update owner
scripts/support_release.py              deterministic exact support asset producer/validator
scripts/support_bootstrap.py            stable pre-import support validator/switch/rollback launcher
scripts/install-macos-app.py           thin Python 3 initial-bootstrap CLI
cmake/macos/Info.plist.in              bundle metadata including the macOS floor
src/                                  owned Qt adaptation surface (see src/ANATOMY.md)
src/ui/                               owned LingTai widgets (see src/ui/ANATOMY.md)
tests/                                owned C++ contracts + Python gates (see tests/ANATOMY.md)
AGENTS.md                             agent-facing build/validate checklist
README.md                             public entry point (build, smoke, dependency boundary)
```

## Build graph (`CMakeLists.txt`)

The single CMake graph materializes ignored `.deps/` inputs, satisfies the
upstream target names the pinned `lib_ui` needs without patching it
(`CMakeLists.txt:79-166`), links the SDK zlib instead of a package-manager
prefix (`CMakeLists.txt:67-70`), and declares the owned libraries:

- `lingtai_desktop_core` — Qt-independent project attachment +
  workspace-selection seam (`CMakeLists.txt`).
- `lingtai_desktop_attachment_selection` — Qt-independent selected-file
  metadata, local preflight, limits, and typed rejection seam (`CMakeLists.txt`).
- `lingtai_desktop_posix_primitives` — descriptor/no-follow primitives, no
  link edge of its own (`CMakeLists.txt:203-209`).
- `lingtai_desktop_agent_projection`, `lingtai_desktop_direct_route`,
  `lingtai_desktop_conversation`, `lingtai_desktop_mail_publisher`,
  `lingtai_desktop_agent_preset_summary`, `lingtai_desktop_agent_setup_store`,
  `lingtai_desktop_preset_catalog`,
  `lingtai_desktop_project_creation`,
  `lingtai_desktop_agent_sleep`,
  `lingtai_desktop_agent_launch` — one library per read/write seam
  (`CMakeLists.txt:211-308`).
- `lingtai_desktop_native_shell` — C5 composition linking the above with
  `desktop-app::lib_ui` (`CMakeLists.txt:176-196`).
- `lingtai_desktop_smoke` — `src/main.cpp` + `src/crl_integration.cpp`
  executable (`CMakeLists.txt:404-410`).

Qt/lib_ui is the only external linked GUI SDK/build dependency. It is
resolved from `QT_ROOT` or the documented `$HOME/Qt/6.11.1/macos` default
(`CMakeLists.txt:13-16`); the app launches only the configured Python kernel
runtime, which is not a linked dependency.

## Owned source owners (`src/`)

`src/main.cpp` is the persistent app entry and owns the `--smoke` path that
emits `LINGTAI_NATIVE_SHELL_READY` and `LINGTAI_LIB_UI_FULL_TARGET_SMOKE_OK`
before exiting (`src/main.cpp:55-83`). `src/crl_integration.cpp` supplies the
bounded no-emission parent `crl` update producer the smoke needs.

`src/shell_host.cpp` owns normal window composition, including the fallback
kernel interpreter.

- `src/native_shell.{h,cpp}` — `NativeShell` (`src/native_shell.h:64`), the
  one C5-owned composition: `WorkspaceSelectionState` C1 truth, the 260px
  `AgentRoster` column, the `ConversationSurface` chat, the one secondary
  read-only page (`AgentDetailPage`, `src/native_shell.h:53-60`), the
  one-second view-scoped `QTimer`, its shared off-thread mailbox snapshot
  generation/index, and the sleep/start pending observations
  (`src/native_shell.h:149-164`). Route into `src/ANATOMY.md`.
- `src/workspace_selection.{h,cpp}` — `WorkspaceSelectionState`
  (`src/workspace_selection.h:20`), the sole active-project/selected-Agent
  transition owner (C1).
- `src/project_attachment.{h,cpp}` — `ProjectAttachment`
  (`src/project_attachment.h:34`) and `attach_project`
  (`src/project_attachment.h:59`): canonical-root containment seam.
- `src/attachment_selection.{h,cpp}` — `preflight_attachments`: read-only
  canonical source metadata, shared deterministic media classification,
  duplicate detection, and ordered 25 MiB / 100 MiB limit application.
- `src/agent_projection.{h,cpp}` — `project_agents`
  (`src/agent_projection.h:89`): one composite `.lingtai` scan returning
  `AgentSnapshot`/`AgentRow`.
- `src/posix_descriptor_primitives.{h,cpp}` — the shared descriptor-anchored,
  one-leaf-at-a-time, `O_NOFOLLOW` walk primitives every reader/publisher
  uses; internal, deliberately not a filesystem framework.
- `src/direct_conversation_route.{h,cpp}` — `resolve_direct_conversation_route`
  and `DirectConversationRoute` (`src/direct_conversation_route.h:23`): pure
  route resolution, no filesystem, no link edge to discovery.
- `src/direct_conversation_history.{h,cpp}` — fixed-count mailbox fingerprint,
  shared all-route snapshot reader/index, and the one-route
  `read_direct_conversation` compatibility projection: read-only mailbox rows
  plus ordered, current-entry-rooted attachment metadata and skip accounting.
- `src/direct_conversation_attachment_actions.{h,cpp}` — fresh current-route,
  current-message-entry no-follow attachment revalidation for Open/Reveal;
  returns a path only after identity match and launches nothing.
- `src/direct_mail_publisher.{h,cpp}` — `send_direct_mail`
  (`src/direct_mail_publisher.h`): one atomically published exclusive human
  outbox leaf with optional private attachment copies.
- `src/agent_preset_summary.{h,cpp}` — `read_agent_preset_summary`
  (`src/agent_preset_summary.h:67`): stateless read-only
  `system/manifest.resolved.json` policy/effective projection.
- `src/preset_catalog.{h,cpp}` — `load_preset_catalog`: bounded, read-only
  saved/template global-library discovery over an injected root, with typed
  directory-read failure and TUI-equivalent ordering.
- `src/agent_setup_store.{h,cpp}` — the UI-independent `AgentSetupStore`
  (`src/agent_setup_store.h:117`): bounded lossless setup load, typed draft,
  TUI-parity preset reconciliation, narrow descriptor-walked configured-env
  merge, peer preset/orchestrator propagation, and staged atomic multi-file
  save/rollback for existing Agents only.
- `src/kanban_model.{h,cpp}` + `src/kanban_page.{h,cpp}` — session-owned
  incremental Kanban source index and its stale-while-revalidate presentation;
  the shell supplies the one coalesced low-priority worker.
- `src/agent_signal.{h,cpp}` — descriptor-relative, no-follow fixed-content
  lifecycle marker writes/removals. `.clear` is exactly `desktop\n`; unsafe
  keys, symlinks, and non-regular leaves fail closed.
- `src/agent_sleep.{h,cpp}` — sleep event baseline/observation plus the
  compatibility `request_agent_sleep` wrapper over `agent_signal`.
- `src/agent_process.{h,cpp}` — exact argv discovery and revalidated TERM/KILL
  for only `python -m lingtai run <canonical-agent-dir>`.
- `src/agent_launch.{h,cpp}` — secure configured-runtime resolution and one
  detached `<python> -m lingtai run <dir>` launch with PID/log facts.
- `src/agent_lifecycle.{h,cpp}` — target/Main/`all` policy and the serial,
  timer-driven `/sleep`, `/suspend`, `/cpr`, `/clear`, `/refresh` state machine,
  including lease waits, preset updates, clear completion, hard-refresh
  escalation, aggregate results, and generation binding.
- `src/project_creation.{h,cpp}` and
  `src/project_creation_resources.{h,cpp}` — `create_project` plus compiled
  Desktop-owned `en`/`zh`/`wen` first-boot content and
  `ProjectCreationRunner`: typed draft/staging/generation/validation/publication
  results, selected-preset prevalidation, descriptor-relative staging,
  in-stage allowed-policy shaping, localized `.prompt`/adaptive `comment.md`,
  exact content/shape and exactly-one-orchestrator validation,
  descriptor-relative rollback before/after marker removal, exclusive atomic
  `.lingtai` publication, and joinable asynchronous delivery. Runtime readiness
  remains a post-commit lifecycle concern.

## Owned UI owners (`src/ui/`)

- `src/ui/agent_roster.{h,cpp}` — `AgentRoster` (`src/ui/agent_roster.h:27`):
  the persistent responsive left project/Agent list column, rows painted from
  the shared lib_ui palette.
- `src/ui/conversation_surface.{h,cpp}` — `ConversationSurface`
  (`src/ui/conversation_surface.h:23`): one read-only, text-selectable,
  bubble-painted conversation surface built from `DirectConversationMessage`
  rows, including ordered bounded thumbnails/file cards and semantic
  Open/Reveal anchors inside each owning message frame.

Both are owned LingTai widgets, not Telegram screens. Route into
`src/ui/ANATOMY.md` for composition details.

## Owned tests (`tests/`)

Each C++ contract executable maps to one ctest name (declared in
`CMakeLists.txt`): `project_attachment`, `attachment_selection`, `agent_projection`,
`direct_conversation_route`, `direct_conversation_history`,
`direct_mail_publisher`,
`agent_preset_summary`, `agent_setup_store`, `agent_signal`, `agent_sleep`,
`agent_process`, `agent_launch`, `agent_lifecycle`,
`posix_descriptor_primitives`,
`workspace_selection`, `native_shell_behavior`, plus the Python gates
`native_shell` (process persistence + smoke-order, `tests/test_native_shell.py`),
`test_repository_contract.py` (pinned toolkit provenance + tracked-artifact
guard), `test_app_archive.py` (portable archive production, independent safe
extraction, exact manifest/App binding, and publication races), and
`test_macos_packaging.py` (optional DMG mode, naming, path, and manifest
contract), `test_desktop_user_cli.py` (App lifecycle and stable/local support
transaction), and `test_desktop_support_update.py` (official support transport,
cache/consent/sequencing, producer, and stage-to-bootstrap commit). Route
into `tests/ANATOMY.md` for the per-test contract mapping.

## Kernel artifacts Desktop reads

Desktop has no link edge into the LingTai kernel package. Its interface to the
kernel is on-disk artifacts under an accepted `.lingtai` project, the exact
configured env leaf named by the selected Agent's bounded `init.json`, plus two
subprocess surfaces. Producer-side contracts live in the kernel repo
(`lingtai.kernel`) and are the normative source for artifact shape:

- `.lingtai/<key>/.agent.json`, `.agent.heartbeat`, `.status.json`,
  `init.json` — read by `agent_projection` / `agent_launch`.
- `.lingtai/<human key>/mailbox/{inbox,sent,outbox}` — read/written by
  `direct_conversation_history` / `direct_mail_publisher`.
- `.lingtai/<key>/logs/events.jsonl` — bounded suffix read by lifecycle sleep
  and clear completion observation.
- `.lingtai/<key>/system/manifest.resolved.json` — read by
  `agent_preset_summary`.
- `.lingtai/<key>/{.sleep,.suspend,.clear,.refresh,.refresh.taken}` — fixed
  lifecycle signal/cleanup leaves; `.agent.lock`, heartbeat, manifest, and
  molt count are observed by `agent_lifecycle`.

## Composition and state

The root composes `src/` (owned Qt adaptation), `tests/` (owned contracts),
`scripts/` + `cmake/` (build governance), and external `.deps/`/Qt inputs.
The primary package boundary turns one already verified self-contained
universal App into a portable tar.gz and exact manifest. Both are completed in
same-filesystem scratch, independently verified, and exclusively hard-linked
into place as a rollback-safe pair. The verifier incrementally preflights every
untrusted tar header before private extraction, rejects payload-bearing links
without advancing past their header, and caps the compressed archive at 512 MiB
(over 20 times the current roughly 23 MiB artifact). It binds archive bytes,
App/executable facts, universal architectures, and the recursive tree digest.
Manifest Git facts identify only the packaging checkout. The older DMG boundary
is an optional diagnostic/release experiment. The canonical bundle/compiled
minimum is macOS 13.0.
The user-level lifecycle acquires stable releases only from the fixed official
`Lingtai-AI/lingtai-desktop` GitHub API/asset hosts, through a per-hop validated
injectable HTTPS boundary, or consumes an explicit local archive pair. Downloads
remain private temporary inputs to the unchanged authoritative App installer.
The App plane binds each `versions/<app-version>` to a current-user-owned,
single-link, mode-0600, bounded canonical receipt and recursive bundle digest,
then switches App `current` only after exclusive publication. Updates use an
atomic exchange that retains the exact observed prior pointer until final
validation, restores a raced or failed switch before new-target cleanup, and
never overwrites a concurrent pointer. Independently, a fresh installation
publishes exactly two mode-0600
payloads under immutable `support/versions/<generation-id>`, a canonical manifest,
mode-0600 state, and support `current`; there is no flat `cli/` layout. A bounded
`initial-install.json` binds the exact App manifest and support generation across
process-death boundaries, while ordinary rollback uses an early directory/inode
ledger and never removes replacements or shared parents. The stable launcher
validates that chain into retained exact bytes before import and resumes only the
exact `pending.json` from->to transaction under the canonical full argv that
staged it. Local staging canonicalizes caller argv0 to the absolute installed
launcher before hashing, publication, and exec, preserving the remaining full-
argv order exactly. It retains the original source symlink under the journal's
random rollback name, publishes failed state before pointer rollback, and
authenticates that inode during replay. An uncommitted target that fails final
identity validation aborts back to that source without trusting target bytes.
Candidate self-test runs in a private minimal-environment child under a no-write/
network/process/native/frame/early-exit audit policy. The wrapper removes the
inherited audit descriptor from candidate argv and replaces candidate-visible
`os.write`/`posix.write` aliases with sticky denial, while its raw terminal writer
and final true-only action remain locals in the active wrapper call. Candidate
`__main__` replacement therefore cannot suppress the genuine final marker, and
injected/partial/extra markers fail the parent's exact singleton-marker grammar.
There is no candidate-mutable ledger. Post-test
generation, pointer, journal, state, and both managed planes are revalidated
before commit. The active CLI stages either an injected local generation or an
exact official generation discovered from the stable GitHub release. Official
support discovery authenticates repository/tag/asset routes and canonical
manifest facts before downloading both declared payloads into one identity-bound
private scratch set; only complete verified Python bytes enter the generalized
immutable generation/pending seam (`scripts/desktop_user_cli.py:648-848`,
`2566-2694`). V1 trust is TLS + official GitHub origin/route + SHA-256, not a
signature claim. The deterministic release-side producer/validator owns the exact
manifest plus two payload files (`scripts/support_release.py:72-144`).
Open and foreground launch only the receipt-validated current App by exact argv.
Normal commands consult the App plane's owned mode-0600 `update-check.json` at most
once per 24 hours: noninteractive calls notice only, and interactive calls use a
default-No offer whose deliberate `y`/`yes` runs the verified update before the
original command continues. The independent canonical
`support/update-check.json` stores support version/tag/generation/manifest/time
and decline. Its own cadence fails open with a warning on corruption/provider
failure, notices only in non-TTY, and prompts default No in TTY; only `y`/`yes`
uses the official staging transaction (`scripts/desktop_user_cli.py:1984-2085`,
`3221-3321`). Explicit official `update` forces support first and then App. A
successful support stage reexecs the same canonical argv; the bootstrap consumes
its marker before import, switches/self-tests/commits, and the injected one-shot
guard resumes App work without looping (`scripts/support_bootstrap.py:1299-1354`,
`scripts/desktop_user_cli.py:3454-3583`). A visible support failure leaves that
plane unchanged and may continue App. Diagnostic local App pairs bypass all
support transport and cache work.
Shared `.local` parents retain existing modes. Managed file modes are prepared
before publication and rollback/removal is identity-bound. Candidate syntax, exact `v<support_version>` numbered release identity,
argv, current/state relationship, anti-rollback, active/failed status, and explicit
retry are validated before immutable generation publication. Paired local App
update and `uninstall --version` do not inspect or mutate support, even when
support is absent, tampered, symlinked, or unknown; every support
stage/switch/rollback does not mutate
App inodes or bytes. `uninstall --all` preflights both complete
planes, including every generation, pointer, state, optional cache/pending, and
the digest-bound stable launcher, before its first deletion.
Persistent Desktop state is deliberately minimal: the one composer outbox
leaf, fixed lifecycle markers, an authorized refresh's atomic active-preset
update, the started Agent's own `logs/` directory, and an explicit setup
transaction's bounded existing-Agent configuration leaves;
the shell performs no implicit project, registry, or settings writes. No Telegram
account, protocol, chat, message, media, contact, or cache state exists here.
