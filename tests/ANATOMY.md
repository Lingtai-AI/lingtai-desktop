# `tests/` anatomy

`tests/` is the repository-owned automated test surface: every automated
proof of Desktop product behavior in this tree lives here, and nothing else
in the tree proves product behavior automatically. It does not replace
human-visible acceptance, which stays outside synthetic/offscreen
automation (see [`tests/BEHAVIORS.md`](BEHAVIORS.md)). The folder is
deliberately small and deliberately flat —
one file per owned CMake target plus focused Python contract gates — and every test
belongs to exactly one of four proof layers: **repository/build/static
contracts**, **pure/domain unit tests**, **real-Qt `native_shell_behavior`**,
or **process-level smoke/persistence**. The layer decides how the test is
run, what it can honestly claim, and what it must not claim. See the
repository map [`../ANATOMY.md`](../ANATOMY.md) for the top-level build
graph, and the source docs [`../src/ANATOMY.md`](../src/ANATOMY.md),
[`../src/CONTRACT.md`](../src/CONTRACT.md), and
[`../src/BEHAVIORS.md`](../src/BEHAVIORS.md) for the production owners each
test proves. This file descends into `tests/` itself.

## Proof layers

### 1. Repository/build/static contracts

- `tests/test_repository_contract.py` — the sole owner of pinned dependency
  provenance and tracked-artifact hygiene: it asserts the exact
  `cmake/desktop-app-toolkit-lock.json` pins (Qt 6.11.1, the
  `tdesktop_commit`, and the seven pinned toolkit commits) and that no
  `.deps/`, `build/`, `Qt/`, or system-temp paths and no leaked
  validation-root marker from lib-ui validation is tracked by git. It is run with
  `python3 -m unittest tests.test_repository_contract`, deliberately not as
  a ctest: it is a repository contract, not a product behavior. Its one
  CMake-source assertion owns the canonical product version; it is not a
  general repository-shape suite.
- `tests/test_app_archive.py` — the offline owner of the primary portable-App
  archive boundary: exact manifest/App-tree binding, safe private extraction,
  executable modes, internal symlink/hardlink preservation, incremental hostile
  member/resource rejection, streaming file digests, malformed/truncated input
  cleanup, and inode-bound pair publication races. It invokes no network, Apple
  service, App, or DMG tool.
- `tests/test_macos_packaging.py` — the offline owner of the optional DMG boundary's
  fail-closed diagnostic/release mode choice, credential-name presence rules,
  deterministic versioned names, unsafe destination/overwrite refusal, tool
  absence, exact-MACOS parser helpers, per-slice otool linkage calls,
  no-clobber pair publication/rollback races, tracked packaging-Git provenance,
  and bounded secret-free manifest shape. It invokes no Apple, GitHub, signing,
  mount, or packaging service.
- `tests/test_desktop_user_cli.py` — the fake-HOME user-install lifecycle
  contract. Its injected transport proves the existing fixed official App
  metadata/assets, HTTPS bounds, cache/TTY offers, and offline failure behavior.
  Its injected platform proves archive verification/extraction, isolated
  60-second ordered-marker smoke, exact open/foreground argv, App receipts and
  pointer safety without Finder, an App, Apple services, or live network.
  The same module's support layer proves canonical exact manifest/state/pending
  bytes, deterministic generation IDs, two-file mode/hash/size/link/type
  validation, high-water and failed-target closure, final no-flat-layout fresh
  install, stable bootstrap delegation, real bounded candidate import/self-test,
  local stage/pending/re-exec, commit and rollback, and pointer/state/pending/
  rollback durable-boundary recovery. It snapshots App bytes and inode identities
  around every support-only operation, proves App-only uninstall preserves
  support, and proves `--all` rejects unknown support before any deletion.
  Authentic filesystem regressions additionally pin shared-parent preservation,
  pre-publication failure, symlink-root containment, and complete no-partial-
  delete behavior for unknown/tampered content in either managed plane.
- Compile-time guards embedded in unit tests: `workspace_selection_test.cpp`
  and `direct_conversation_route_test.cpp` start with
  `#ifdef QT_CORE_LIB / #error` so a Qt-core dependency leaking into a
  Qt-free consumer fails the build, and `posix_descriptor_primitives_test.cpp`
  does the same for the internal seam. `static_assert`s pin `noexcept`
  signatures and exact return types (`workspace_selection_test.cpp:261-269`,
  `project_attachment_test.cpp:36`, `direct_conversation_route_test.cpp:99`).
  Every owned test target compiles under
  `-Wall -Wextra -Werror -pedantic` (`CMakeLists.txt:310-374`).

### 2. Pure/domain unit tests (no Qt, no network, injected fixture roots)

The majority of `tests/`: each compiles against exactly one owned library
target, takes one fixture-root argument from ctest, builds its own project
tree inside that root, and proves a bounded read/write/derivation contract.
Most readers in this layer are Qt-free; `kanban_model` deliberately links
QtCore for the production JSON/date projection but no widget stack. No test
touches a real Agent or project, and none depends on a network or provider.

- `tests/posix_descriptor_primitives_test.cpp` — `posix_descriptor_primitives`
  ctest. Move-only descriptor/directory-stream ownership, shared read flags,
  `safe_leaf` validation, one-leaf-at-a-time opens.
- `tests/project_attachment_test.cpp` — `project_attachment` ctest.
  Containment seam: canonical roots, typed failures, symlink/no-escape rules.
- `tests/attachment_selection_test.cpp` — `attachment_selection` ctest.
  Qt-free direct-file metadata, typed local rejection, deduplication, ordered
  size limits, shared classifier, accounting, and no-mutation proof.
- `tests/workspace_selection_test.cpp` — `workspace_selection` ctest. C1
  model: activation/selection/clear transitions, no-write proof.
- `tests/agent_projection_test.cpp` — `agent_projection` ctest. The one
  composite discovery/role/presence/status projection.
- `tests/direct_conversation_route_test.cpp` — `direct_conversation_route`
  ctest. Pure route/eligibility derivation; no filesystem access.
- `tests/slash_command_test.cpp` — `slash_command` ctest. Pure TUI-shaped
  command classification; no Qt, filesystem, project fixture, or side effect.
- `tests/direct_conversation_history_test.cpp` — `direct_conversation_history`
  ctest. Read-only mailbox rows and attachment metadata, descriptor-relative
  containment, shared multi-route one-parse projection, fixed-count
  fingerprints, deterministic generation/race/reset state, independent skip
  accounting, membership, order, collapse, and no-write proof.
- `tests/direct_conversation_attachment_actions_test.cpp` —
  `direct_conversation_attachment_actions` ctest. Fresh current-message-entry
  lookup, descriptor-relative no-follow reopen, identity match, and mutation /
  escape failure matrix.
- `tests/direct_mail_publisher_test.cpp` — `direct_mail_publisher` ctest.
  Atomic text/attachment outbox leaf, revalidation, limits, final sent paths,
  naming, private byte copies, typed failures, rollback, and containment.
- `tests/agent_preset_summary_test.cpp` — `agent_preset_summary` ctest.
  `resolved`/`stale`/`unavailable` projection of the selected Agent's
  `manifest.resolved.json`.
- `tests/agent_setup_store_test.cpp` — `agent_setup_store` ctest. Full-document
  preservation, Keep Current/TUI preset reconciliation, absolute external-env
  ON/OFF merge, empty-policy no-propagation, peer orchestrator privilege
  boundaries, unsafe/oversize/type rejection, absent-optional-path no-op bytes,
  and staged/publish rollback against one injected fixture root.
- `tests/preset_catalog_test.cpp` — `preset_catalog` ctest. Hermetic
  saved/template library facts, skip rules, canonical order, missing-directory
  success, typed directory-read failure, ref normalization, and no-write.
- `tests/project_creation_test.cpp` — `project_creation` ctest. Exact project
  shape and stable de-duplicated setup policy, saved/template controlled
  manifest projection, legacy/provider-matched capability normalization,
  hash-pinned `en`/`zh`/`wen` adaptive source provenance plus independently
  stated Desktop adaptations with deterministic injected time/location, exact
  three-language output, rejection of TUI-only prose and backticked commands
  outside the public Desktop slash registry, generated-placeholder elimination,
  byte-exact reviewed comment plus final manifest reference, correct project-root
  `.recipe`/`.tui-asset` absence, exact no-global-mutation snapshots,
  runtime-independent publication, staged validation,
  typed asynchronous stage/detail delivery, exclusive publication, descriptor
  rollback across generation/marker-removal/publish refusal, join-on-
  destruction callback suppression, conflicting state preservation, and
  no-follow path rejection under one injected root.
- `tests/test_project_creation_source_contract.py` — manual Python unittest
  proving production rollback has descriptor primitives and no path-recursive
  `remove_all` call, TUI adapter, creation-time runtime-readiness gate, network
  location resolver, global guidance write, or statically embedded impossible
  Desktop guidance; it also verifies all seven pinned adaptive fixture hashes
  and the compiled bounded Desktop-owned content target.
- `tests/agent_sleep_test.cpp` — `agent_sleep` ctest. Exact-target `.sleep`
  marker write plus the baseline/observe pair.
- `tests/agent_lifecycle_test.cpp` — `agent_lifecycle` ctest. Deterministic
  signal/path safety, target/argument matrix, advisory leases, exact-process
  matching and escalation, every lifecycle flow/timeout, preset validation,
  clear completion, aggregate results, and generation binding.
- `tests/agent_lifecycle_real_smoke.cpp` — manually invoked acceptance harness.
  Drives Desktop creation, attachment/setup, the production controller,
  process adapter, and detached launcher against disposable real kernel
  Agents under a fake HOME and TUI-free PATH.
- `tests/kanban_model_test.cpp` — `kanban_model` ctest. Full semantic facts
  plus deterministic incremental counters for unchanged cycles, four growing
  JSONL sources, full-read/cursor-capture races, partial/reset handling,
  capture-incapable cursor liveness, human-row follow-up suppression, 300-run
  daemon inventory changes, SQLite session freshness, malformed/unsafe parity,
  and same-mtime newest-128 ordering.

### 3. Real-Qt widget/shell contracts

- `tests/native_shell_test.cpp` — the split `native_shell_<journey>` ctests.
  The real-Qt shell contract links `lingtai_desktop_native_shell` +
  `desktop-app::lib_ui` + `src/crl_integration.cpp` (`CMakeLists.txt:837-849`),
  constructs a real `QApplication`, shows the real `NativeShell`
  off-screen (`shell.show_offscreen()`), and drives the real widgets to
  prove shell semantics, named regions, geometry, the composer send flow,
  the functional composer paste/IME flow, the dashboard sections, Request
  sleep, Start Agent, layout modes, live
  light→dark→light and dark→light→dark stored-control palette refresh, and
  the no-write rule. On macOS it runs with `QT_QPA_PLATFORM=cocoa`, elsewhere
  with `offscreen` (`CMakeLists.txt:873-879`). CMake creates and passes one
  `native-shell-<journey>-fixture` working root per split journey
  (`CMakeLists.txt:853-872`). `project_tree` snapshots
  surround the specific read-only, no-escape, and no-write cases — and any
  explicit outside symlink target the test names — proving those fixtures
  remain unchanged through those operations; the write journeys (composer
  send/outbox, Request sleep marker, New Project/bootstrap) instead assert
  the exact intended in-fixture mutations on the synthetic
  `commit-N-...-fixture` trees the test itself creates. The working
  directory is an injected path, not an OS or process sandbox.
- Its focused `native_shell_new_window_bootstrap` path commits literal `~`,
  `~/Documents`, `~/Documents/`, `~/Documents///`, and an absolute destination
  ending in `/` through real `Ui::InputField` editors, then activates the real
  Review Create buttons to prove exact fake-HOME/root normalization, terminal-
  separator acceptance, unchanged non-separator bytes, rejection of traversal,
  dot, missing-leaf, and symlink paths, visible draft-preserving
  pre-publication recovery/retry, no rejected-input publication, and unchanged
  attach/launch handoff after success. These input-method events do not
  automate a native `QFileDialog`, literal system Command-V, or submission in
  the packaged physical App; those remain acceptance gates.
- Its focused `native_shell_kanban` journey holds and fails the real worker
  seam to prove warm updating, stale retention, Reload coalescing, and
  old-project generation rejection.
- Its focused `setup` journey opens one synthetic selected Agent and proves
  the visible full saved/template catalog and normalized preselection, fallback
  addition only for unresolved refs, shared saved/template editor journeys,
  reference-safe policy/unknown-active round-trip, all three step/Back routes,
  direct store save, immutable identity, no-op/cancel bytes, typed failures,
  selection-preserving refresh, no TUI invocation, and both mode resets.
- Its focused `outgoing` journey holds the real mailbox worker behind a stale
  generation and proves synchronous text/attachment presentation, rapid-send
  order, Agent isolation, transient-snapshot retention, authoritative
  deduplication, and pending-row retirement through the ordinary Send/render
  path.
- The conversation journey's composer proof owns attachment selection,
  pending cards, no-follow/revalidated bounded thumbnail fallback, warnings,
  attachment-only publication, exact indexed versus general-failure draft
  retention, slash isolation, and target clearing. Its picker is injected, so
  no test opens a native modal or Finder.
- Its focused `native_shell_paste` journey carries plain and rich-source
  logical text in `QMimeData`, delivers it through real Qt drag/drop events to
  the `Ui::InputField` MIME insertion path, then drives its real input-method
  emoji path and proves selection/caret editing,
  Unicode Send/render/clear, and one application-owned emoji runtime shared by
  two simultaneous shells. On macOS it uses Cocoa without ever accessing
  `QClipboard`; literal system paste remains outside automation because the
  full shell cannot run on the offscreen/minimal Qt plugins
  (`tests/native_shell_test.cpp:2557`, `CMakeLists.txt:863-879`).
- The same conversation journey injects the history attachment external-action
  seam and proves exact Open/Reveal invocation after refresh, no invocation on
  a missing file, transient notices, and preservation of history text, scroll,
  and composer draft. It never launches Finder or another application.
- `tests/native_shell_destinations_test.cpp` — `native_shell_destinations`
  ctest. The one focused Repair3 destination contract: it links the same
  shell + `desktop-app::lib_ui` + `src/crl_integration.cpp`
  (`CMakeLists.txt:444-464`), constructs a fresh real `QApplication` and
  `NativeShell` with no project and no external fixture, and proves only
  that the removed Activity and Task Card destinations have no surviving
  page-nav button, panel surface, heading, state line, or section owner,
  that the page navigation retains exactly Conversation + Presets, and that
  the low-level `lingtai_selected_agent_status_activity` fact label stays
  present with its stable identity. It runs with the platform Qt plugin like
  `native_shell_behavior`.
- `tests/native_shell_presets_test.cpp` — `native_shell_presets` ctest. The
  one dedicated Repair4 Presets presentation contract: it links the same
  shell + `desktop-app::lib_ui` + `src/crl_integration.cpp`
  (`CMakeLists.txt:466-489`), constructs a fresh real `QApplication` and
  `NativeShell`, and proves that the selected Agent's Resolved Presets page
  shows exactly the minimal Provider/Model/Default/Allowed text, and that the
  removed active-ref/badge/context/capability/source-provenance text and the
  six generic fact widgets are absent. It runs with the platform Qt plugin
  like `native_shell_behavior`.

- `tests/conversation_surface_typography_test.cpp` —
  `conversation_surface_typography` ctest. The dedicated focused widget/
  document typography contract: it compiles `src/ui/conversation_surface.cpp`
  plus the test (`CMakeLists.txt:493-516`), builds a real `QApplication` and
  `ConversationSurface`, renders representative incoming and outgoing
  messages, and proves the distinct author 15px DemiBold / body 14px Normal /
  timestamp 13px Normal / subject 13px Medium fragments (author >= body >
  metadata) with the exact 15/14/13 pixel values. Its attachment coverage sends
  real viewport press/move/release events to distinguish exact action clicks
  from native text-selection drags. It takes no fixture root.
- `tests/conversation_surface_scroll_test.cpp` —
  `conversation_surface_scroll` ctest. A focused real-Qt surface-only contract:
  it constructs `QApplication` and production `ConversationSurface`, sends
  real `QWheelEvent`s to the viewport, and proves zero-delta queued-pin
  cancellation, tiny-update and momentum ownership, non-jumping `ScrollEnd`,
  ordinary bottom-follow, manual non-bottom preservation, and delegation to
  native `QTextEdit` wheel movement. It does not construct `AgentDetailView`
  or `NativeShell` and takes no fixture root.

### 4. Process-level smoke/persistence

- `tests/test_native_shell.py` — `native_shell` ctest. Runs the built
  `lingtai_desktop_smoke` executable (`src/main.cpp` + `src/crl_integration.cpp`,
  `CMakeLists.txt:404-410`) via `$<TARGET_FILE:lingtai_desktop_smoke>`
  (`CMakeLists.txt:412-416`) as a real subprocess: `--smoke` proves the
  ordered readiness/success markers and named regions on the real shell,
  and `--offscreen` proves normal mode persists (does not exit). The
  `./scripts/smoke.py` wrapper runs the same `--smoke` with the Qt plugin
  path set and an 8 s timeout.

## CMake/ctest mapping

Each ctest registers one executable (or Python script) against one fixture
path under the build directory (or, for the fixture-less typography contract,
no fixture); the test itself creates and removes its sandbox within that root
(`CMakeLists.txt:375-535`):

| Test file | Target / executable | ctest name | Fixture (build dir) |
| --- | --- | --- | --- |
| `test_repository_contract.py` | Python `unittest` (no target) | manual: `python3 -m unittest tests.test_repository_contract` | — |
| `test_project_creation_source_contract.py` | Python `unittest` (no target) | manual: `python3 -m unittest tests.test_project_creation_source_contract` | — |
| `test_app_archive.py` | Python `unittest` (no target) | manual: `python3 -m unittest tests.test_app_archive` | temporary injected directories only |
| `test_macos_packaging.py` | Python `unittest` (no target) | manual: `python3 -m unittest tests.test_macos_packaging` | temporary injected directories only |
| `test_desktop_user_cli.py` | Python `unittest` (no target) | manual: `python3 -m unittest tests.test_desktop_user_cli` | fake HOME + injected transport/platform/clock/TTY/prompt boundaries |
| `posix_descriptor_primitives_test.cpp` | `lingtai_posix_descriptor_primitives_test` | `posix_descriptor_primitives` | `posix-descriptor-primitives-fixture` |
| `project_attachment_test.cpp` | `lingtai_project_attachment_test` | `project_attachment` | `project-attachment-fixture` |
| `attachment_selection_test.cpp` | `lingtai_attachment_selection_test` | `attachment_selection` | `attachment-selection-fixture` |
| `workspace_selection_test.cpp` | `lingtai_workspace_selection_test` | `workspace_selection` | `workspace-selection-fixture` |
| `agent_projection_test.cpp` | `lingtai_agent_projection_test` | `agent_projection` | `agent-projection-fixture` |
| `direct_conversation_route_test.cpp` | `lingtai_direct_conversation_route_test` | `direct_conversation_route` | `direct-conversation-route-fixture` |
| `direct_conversation_history_test.cpp` | `lingtai_direct_conversation_history_test` | `direct_conversation_history` | `direct-conversation-history-fixture` |
| `direct_conversation_attachment_actions_test.cpp` | `lingtai_direct_conversation_attachment_actions_test` | `direct_conversation_attachment_actions` | `direct-conversation-attachment-actions-fixture` |
| `direct_mail_publisher_test.cpp` | `lingtai_direct_mail_publisher_test` | `direct_mail_publisher` | `direct-mail-publisher-fixture` |
| `agent_preset_summary_test.cpp` | `lingtai_agent_preset_summary_test` | `agent_preset_summary` | `agent-preset-summary-fixture` |
| `agent_setup_store_test.cpp` | `lingtai_agent_setup_store_test` | `agent_setup_store` | `agent-setup-store-fixture` |
| `project_creation_test.cpp` | `lingtai_project_creation_test` | `project_creation` | `project-creation-fixture` |
| `agent_sleep_test.cpp` | `lingtai_agent_sleep_test` | `agent_sleep` | `agent-sleep-fixture` |
| `agent_lifecycle_test.cpp` | `lingtai_agent_lifecycle_test` | `agent_lifecycle` | `agent-lifecycle-fixture` |
| `agent_lifecycle_real_smoke.cpp` | `lingtai_agent_lifecycle_real_smoke` | manual only | caller-owned isolated project root |
| `kanban_model_test.cpp` | `lingtai_kanban_model_test` | `kanban_model` | `kanban-model-fixture` |
| `native_shell_test.cpp` | `lingtai_native_shell_test` | split `native_shell_<journey>` tests, including `native_shell_lifecycle` | per-journey CMake fixture |
| `native_shell_test.cpp` (paste journey) | `lingtai_native_shell_test` | `native_shell_paste` | `native-shell-paste-fixture` (CMake-created) |
| `native_shell_destinations_test.cpp` | `lingtai_native_shell_destinations_test` | `native_shell_destinations` | — |
| `native_shell_presets_test.cpp` | `lingtai_native_shell_presets_test` | `native_shell_presets` | `native-shell-presets-fixture` (CMake-created) |
| `conversation_surface_typography_test.cpp` | `lingtai_conversation_surface_typography_test` | `conversation_surface_typography` | — (no fixture) |
| `conversation_surface_scroll_test.cpp` | `lingtai_conversation_surface_scroll_test` | `conversation_surface_scroll` | — (no fixture) |
| `test_native_shell.py` | `lingtai_desktop_smoke` (built) | `native_shell` | `$<TARGET_FILE:lingtai_desktop_smoke>` |

## Fixture and data ownership

- Every domain ctest passes its own `CMAKE_CURRENT_BINARY_DIR/<name>-fixture`
  path as the sole argv argument; the test `remove_all`s it, rebuilds a
  synthetic project tree, and `remove_all`s it again at the end. Expected
  test writes stay under the injected fixture trees by dependency and path
  injection — the fixture root is the only path handed to the code under
  test — and no test touches a real `.lingtai` project, real Agent,
  registry, or settings file.
- The fixture trees are exact byte/type images (shared `tree_snapshot`
  helpers in `direct_conversation_history_test.cpp:41`,
  `direct_mail_publisher_test.cpp:40`,
  `agent_preset_summary_test.cpp:44`,
  `agent_sleep_test.cpp:51`), so "reading/writing never mutates" is proven
  against the real tree: the snapshot establishes that the snapshotted
  fixture — and any explicit outside symlink target the test names, e.g.
  `outside_before` — remains unchanged. It is not an OS or process sandbox
  and does not claim nothing can leak to an arbitrary unobserved outside
  path.
- The Qt-aware tests are not run under the domain layer: `native_shell_test`,
  `conversation_surface_typography_test`, and
  `conversation_surface_scroll_test` need the `lib_ui` link edge and
  `test_native_shell.py` needs the built smoke executable, so all are
  registered as their own ctests and run with the platform Qt plugin.
- Production ownership is deliberately not duplicated here: the readers,
  writers, and the C1 model are all named and owned in
  [`../src/ANATOMY.md`](../src/ANATOMY.md). This file records only the test
  side of each edge — which file proves which target, with which ctest name.

## Notes

- The repository and packaging Python contracts are deliberate non-ctests:
  keeping them as manual `python3 -m unittest` gates keeps product behavior
  purely CMake/ctest-driven while still guarding provenance, tracked artifacts,
  and offline packaging invariants on every validation pass (`../AGENTS.md`).
- The `-Wall -Wextra -Werror -pedantic` flags apply to every owned test
  target, so a Qt dependency sneaking into a Qt-free consumer (or any
  unused/ambiguous construct) fails at compile time, not at runtime.
- `native_shell_behavior` runs with the CMake-created fixture directory as
  `std::filesystem::current_path` (`native_shell_test.cpp:2928`) and
  compares `project_tree` around the specific read-only, no-escape, and
  no-write cases, proving the snapshotted fixture (and any explicit outside
  symlink target the test names) remains unchanged through those operations;
  the send/sleep/bootstrap write journeys assert the exact intended
  in-fixture mutations instead. The working directory is an injected path,
  not an OS or process sandbox.
