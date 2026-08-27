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
scripts/macos_packaging.py             testable fail-closed macOS package owner
scripts/package-macos.py               diagnostic/release package CLI
scripts/verify-macos-package.py        independent mounted-DMG/linkage/smoke verifier
scripts/desktop_user_cli.py            user install/update/launch lifecycle policy
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
  `lingtai_desktop_tui_executable`,
  `lingtai_desktop_agent_sleep`,
  `lingtai_desktop_agent_launch` — one library per read/write seam
  (`CMakeLists.txt:211-308`).
- `lingtai_desktop_native_shell` — C5 composition linking the above with
  `desktop-app::lib_ui` (`CMakeLists.txt:176-196`).
- `lingtai_desktop_smoke` — `src/main.cpp` + `src/crl_integration.cpp`
  executable (`CMakeLists.txt:404-410`).

Qt/lib_ui is the only external linked GUI SDK/build dependency. It is
resolved from `QT_ROOT` or the documented `$HOME/Qt/6.11.1/macos` default
(`CMakeLists.txt:13-16`); the app also invokes TUI and Python subprocess
surfaces at runtime, which are not linked dependencies.

## Owned source owners (`src/`)

`src/main.cpp` is the persistent app entry and owns the `--smoke` path that
emits `LINGTAI_NATIVE_SHELL_READY` and `LINGTAI_LIB_UI_FULL_TARGET_SMOKE_OK`
before exiting (`src/main.cpp:55-83`). `src/crl_integration.cpp` supplies the
bounded no-emission parent `crl` update producer the smoke needs.

`src/shell_host.cpp` owns normal window composition, including the fallback
interpreter and the configured TUI executable. `src/tui_executable_resolver.*`
owns the injected PATH → managed receipt → home-local → two system-directory
resolution order and preserves an accepted symlink path.

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
- `src/project_bootstrap.{h,cpp}` — `ProjectBootstrapRunner`
  (`src/project_bootstrap.h:54`): async exact-argv `presets`/`spawn` headless
  TUI calls + JSON parse.

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
guard), and `test_macos_packaging.py` (offline package-mode, naming, path, and
manifest contract). Route
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
The package boundary always stages a copy: diagnostic mode embeds Qt and
ad-hoc signs only, while release mode fails closed unless Developer ID signing,
hardened runtime, timestamping, notarization, stapling, and strict independent
verification all succeed. DMG and manifest are completed in same-filesystem
scratch and exclusively hard-linked into place as a rollback-safe pair. The
independent verifier checks linkage per exact arm64 and x86_64 slice. Manifest
Git facts identify only the packaging checkout. The canonical bundle/compiled
minimum is macOS 13.0.
The user-level lifecycle consumes one already-built local pair. It installs
under `$HOME/.local`, carries the independent verifier with the managed CLI,
binds each version to a bounded receipt and deterministic bundle-tree digest,
and switches the relative `current` symlink only after exclusive publication.
Open and foreground launch only that receipt-validated current App by exact
argv. Diagnostic opt-in preserves the diagnostic classification.
Shared `.local` parents retain existing modes. Managed file modes are prepared
before exclusive publication, rollback identity is inode-bound, and uninstall
uses a complete exact-child/receipt/digest/ancestor preflight before mutation.
Persistent Desktop state is deliberately minimal: the one composer outbox
leaf, fixed lifecycle markers, an authorized refresh's atomic active-preset
update, the started Agent's own `logs/` directory, and an explicit setup
transaction's bounded existing-Agent configuration leaves;
the shell performs no implicit project, registry, or settings writes. No Telegram
account, protocol, chat, message, media, contact, or cache state exists here.
