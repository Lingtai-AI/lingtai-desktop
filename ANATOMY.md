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

- `lingtai_desktop_core` — Qt-independent attachment + workspace-selection seam
  (`CMakeLists.txt:169-174`).
- `lingtai_desktop_posix_primitives` — descriptor/no-follow primitives, no
  link edge of its own (`CMakeLists.txt:203-209`).
- `lingtai_desktop_agent_projection`, `lingtai_desktop_direct_route`,
  `lingtai_desktop_conversation`, `lingtai_desktop_mail_publisher`,
  `lingtai_desktop_agent_preset_summary`, `lingtai_desktop_agent_sleep`,
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

`src/main.cpp` is the persistent app entry: one `NativeShell`, the one
Desktop fallback interpreter (`set_agent_start_fallback_python`,
`src/main.cpp:25-27`), the one configured TUI executable resolved from PATH
(`set_tui_executable`, `src/main.cpp:43-48`), and the `--smoke` path that
emits `LINGTAI_NATIVE_SHELL_READY` and `LINGTAI_LIB_UI_FULL_TARGET_SMOKE_OK`
before exiting (`src/main.cpp:55-83`). `src/crl_integration.cpp` supplies the
bounded no-emission parent `crl` update producer the smoke needs.

- `src/native_shell.{h,cpp}` — `NativeShell` (`src/native_shell.h:64`), the
  one C5-owned composition: `WorkspaceSelectionState` C1 truth, the 260px
  `AgentRoster` column, the `ConversationSurface` chat, the one secondary
  read-only page (`AgentDetailPage`, `src/native_shell.h:53-60`), the
  one-second view-scoped `QTimer`, and the sleep/start pending observations
  (`src/native_shell.h:149-164`). Route into `src/ANATOMY.md`.
- `src/workspace_selection.{h,cpp}` — `WorkspaceSelectionState`
  (`src/workspace_selection.h:20`), the sole active-project/selected-Agent
  transition owner (C1).
- `src/project_attachment.{h,cpp}` — `ProjectAttachment`
  (`src/project_attachment.h:34`) and `attach_project`
  (`src/project_attachment.h:59`): canonical-root containment seam.
- `src/agent_projection.{h,cpp}` — `project_agents`
  (`src/agent_projection.h:89`): one composite `.lingtai` scan returning
  `AgentSnapshot`/`AgentRow`.
- `src/posix_descriptor_primitives.{h,cpp}` — the shared descriptor-anchored,
  one-leaf-at-a-time, `O_NOFOLLOW` walk primitives every reader/publisher
  uses; internal, deliberately not a filesystem framework.
- `src/direct_conversation_route.{h,cpp}` — `resolve_direct_conversation_route`
  and `DirectConversationRoute` (`src/direct_conversation_route.h:23`): pure
  route resolution, no filesystem, no link edge to discovery.
- `src/direct_conversation_history.{h,cpp}` — `read_direct_conversation`
  (`src/direct_conversation_history.h:41`): read-only mailbox rows.
- `src/direct_mail_publisher.{h,cpp}` — `send_direct_mail`
  (`src/direct_mail_publisher.h:22`): one exclusive human outbox leaf.
- `src/agent_preset_summary.{h,cpp}` — `read_agent_preset_summary`
  (`src/agent_preset_summary.h:67`): stateless read-only
  `system/manifest.resolved.json` policy/effective projection.
- `src/agent_sleep.{h,cpp}` — `request_agent_sleep` /
  `capture_agent_sleep_event_baseline` / `observe_agent_sleep_received`
  (`src/agent_sleep.h:24-51`): one `.sleep` leaf write + best-effort
  observation.
- `src/agent_launch.{h,cpp}` — `start_agent` (`src/agent_launch.h:26`): one
  detached `<python> -m lingtai run <dir>` start.
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
  rows.

Both are owned LingTai widgets, not Telegram screens. Route into
`src/ui/ANATOMY.md` for composition details.

## Owned tests (`tests/`)

Each C++ contract executable maps to one ctest name (declared
`CMakeLists.txt:375-442`): `project_attachment`, `agent_projection`,
`direct_conversation_route`, `direct_conversation_history`,
`direct_mail_publisher`,
`agent_preset_summary`, `agent_sleep`, `posix_descriptor_primitives`,
`workspace_selection`, `native_shell_behavior`, plus the Python gates
`native_shell` (process persistence + smoke-order, `tests/test_native_shell.py`)
and `test_repository_contract.py` (pinned toolkit provenance + tracked-artifact
guard, run via `python3 -m unittest tests.test_repository_contract`). Route
into `tests/ANATOMY.md` for the per-test contract mapping.

## Kernel artifacts Desktop reads

Desktop has no link edge into the LingTai kernel package. Its only interface
to the kernel is on-disk artifacts under an accepted `.lingtai` project plus
two subprocess surfaces. Producer-side contracts live in the kernel repo
(`lingtai.kernel`) and are the normative source for artifact shape:

- `.lingtai/<key>/.agent.json`, `.agent.heartbeat`, `.status.json`,
  `init.json` — read by `agent_projection` / `agent_launch`.
- `.lingtai/<human key>/mailbox/{inbox,sent,outbox}` — read/written by
  `direct_conversation_history` / `direct_mail_publisher`.
- `.lingtai/<key>/logs/events.jsonl` — read by `agent_sleep` (sleep
  observation) as the selected Agent's own event journal.
- `.lingtai/<key>/system/manifest.resolved.json` — read by
  `agent_preset_summary`.
- `.lingtai/<key>/.sleep` — the one sleep leaf written by `agent_sleep`.

## Composition and state

The root composes `src/` (owned Qt adaptation), `tests/` (owned contracts),
`scripts/` + `cmake/` (build governance), and external `.deps/`/Qt inputs.
Persistent Desktop state is deliberately minimal: the one composer outbox
leaf, the one `.sleep` marker, and the started Agent's own `logs/` directory;
the shell performs no project, registry, or settings writes. No Telegram
account, protocol, chat, message, media, contact, or cache state exists here.
