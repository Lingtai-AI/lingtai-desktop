# LingTai Desktop Behaviors

This file states only project-level observable invariants — things you can
observe from the repository root without reading a specific owner — and the
routing from each invariant to the test/oracle that proves it. It does not
restate per-owner behavior; owner-level detail lives in `src/`, `src/ui/`, and
`tests/` child docs.

## Oracle classification

- **Telegram** is the *visual/mature-interaction oracle only*: Desktop
  compares layout and interaction maturity against Telegram's rendered
  behavior, and never imports Telegram account, protocol, chat, message,
  media, contact, cache, or high-level UI code, and never links or depends on
  the Telegram product.
- **TUI** (`lingtai-tui`) is a *functional reference*, not a runtime
  dependency: compatible on-disk behavior may be compared against it, while
  Desktop owns project creation, setup, lifecycle, and launch directly.
- **Desktop** is the *Qt adaptation*: it adapts Qt 6.11.1 + the pinned
  `desktop-app::lib_ui` to LingTai's on-disk project model, owning none of
  Telegram's or the kernel's protocol/business logic.

## Project-level invariants

- **Repro-1 — Smoke marker order.** `lingtai_desktop_smoke --smoke` emits
  `LINGTAI_NATIVE_SHELL_READY` then `LINGTAI_LIB_UI_FULL_TARGET_SMOKE_OK`
  before exiting 0 under a 3 s timeout; readiness failure exits 98 and timeout
  exits 99. Proven by `tests/test_native_shell.py` (ctest name `native_shell`)
  and `scripts/smoke.py`.
- **Repro-1a — One process status item without resident mode.** Normal Desktop
  execution creates one process-owned macOS status item after the first shell,
  with exactly **Show LingTai**, a separator, and **Quit LingTai**. Show restores
  and activates the most recently active still-owned shell (or the deterministic
  remaining fallback); Quit follows Qt's application-quit path. Closing the
  final window still quits, and opening or closing other windows never creates a
  second item. Proven by the three focused `native_shell_status_item*` /
  `native_shell_final_window_quit` ctests and
  `tests/test_desktop_status_item_contract.py`.
- **Repro-2 — Pinned release inputs.** The canonical product version is v0.1.10,
  and the exact toolkit, third-party, Qt, and tdesktop-comparison commits in
  `cmake/desktop-app-toolkit-lock.json` never drift; no `.deps/`, `build/`,
  or `Qt/` tree is ever tracked. Proven by `tests/test_repository_contract.py`
  (`python3 -m unittest tests.test_repository_contract`).
- **Repro-3 — Qt adaptation, not Telegram fork.** No source file in `src/` or
  `tests/` imports or links Telegram account/protocol/chat/media/contact/cache
  code; `desktop-app::lib_ui` is built unmodified from locked sources. Guarded
  by `AGENTS.md` scope rule and the `lingtai_desktop_native_shell` link set in
  `CMakeLists.txt`; enforced at review, not by a dedicated test.
- **Repro-4 — Kernel boundary is filesystem + exact runtime process only.** The
  repository never links the `lingtai` Python package; kernel contact is
  exclusively on-disk `.lingtai` artifacts, advisory lock/process observation,
  and exact-argv `python -m lingtai run`. No production path executes a TUI.
  Enforced by link graph
  (`CMakeLists.txt:169-308`) and review.
- **Repro-4a — New Project does not require TUI availability.** Desktop reads
  the injected global preset catalog, commits the project transaction, attaches
  it, and launches through its lifecycle owner while `PATH` contains no TUI.
  Proven by `project_creation`, `native_shell_bootstrap`, and the real smoke.
- **Repro-5 — Read seams write nothing.** Every read projection
  (`project_agents`, `read_direct_conversation`,
  `read_agent_preset_summary`,
  `resolve_direct_conversation_route`) is `noexcept` and writes no project
  tree, proven per seam by its C++ contract test below.
- **Repro-6 — No pending-lifecycle lies.** A lifecycle result observed after a
  project/selection generation change can never surface under the later
  selection. Proven by `agent_lifecycle` and `native_shell_lifecycle`.
- **Repro-7 — Official updates are never silent.** Normal installed commands
  use one bounded 24-hour cache: noninteractive invocations notice only, and an
  interactive invocation installs only after deliberate `y`/`yes`, then
  continues its original command against the verified current App. Explicit
  `update` forces fresh official stable-release discovery without prompting;
  uninstall never offers. Before publication, the staged App gets the exact
  isolated environment and `--smoke` invocation with a 60-second ceiling and
  ordered ready/full-target markers. The fixed GitHub source, hostile response
  rejection, smoke failure closure, cache ownership, rollback, and fake-HOME
  behavior are proven offline by `tests/test_desktop_user_cli.py`.

## Test / oracle routing

| Invariant | Proof | Command |
|---|---|---|
| Repro-1 smoke marker order + exit codes | `tests/test_native_shell.py` (`native_shell`) | `ctest --test-dir build -R '^native_shell$'` |
| Repro-1 real offscreen shell | `tests/native_shell_test.cpp` (`native_shell_behavior`) | `ctest --test-dir build -R '^native_shell_behavior$'` |
| Repro-1a process status item | `tests/native_shell_test.cpp`; `tests/test_desktop_status_item_contract.py` | `ctest --test-dir build -R '^(desktop_status_item_source_contract|native_shell_status_item|native_shell_status_item_quit|native_shell_final_window_quit)$'` |
| Repro-2 lock provenance + v0.1.10 + hygiene | `tests/test_repository_contract.py` | `python3 -m unittest tests.test_repository_contract` |
| Core attachment/containment | `tests/project_attachment_test.cpp` (`project_attachment`) | `ctest --test-dir build -R '^project_attachment$'` |
| Direct attachment selection facts | `tests/attachment_selection_test.cpp` (`attachment_selection`) | `ctest --test-dir build -R '^attachment_selection$'` |
| C1 selection transitions | `tests/workspace_selection_test.cpp` (`workspace_selection`) | `ctest --test-dir build -R '^workspace_selection$'` |
| Descriptor/no-follow primitives | `tests/posix_descriptor_primitives_test.cpp` (`posix_descriptor_primitives`) | `ctest --test-dir build -R '^posix_descriptor_primitives$'` |
| Composite agent projection | `tests/agent_projection_test.cpp` (`agent_projection`) | `ctest --test-dir build -R '^agent_projection$'` |
| Pure route eligibility | `tests/direct_conversation_route_test.cpp` (`direct_conversation_route`) | `ctest --test-dir build -R '^direct_conversation_route$'` |
| Mailbox read contract | `tests/direct_conversation_history_test.cpp` (`direct_conversation_history`) | `ctest --test-dir build -R '^direct_conversation_history$'` |
| Attachment action revalidation | `tests/direct_conversation_attachment_actions_test.cpp` (`direct_conversation_attachment_actions`) | `ctest --test-dir build -R '^direct_conversation_attachment_actions$'` |
| Outbox publisher (TUI envelope oracle) | `tests/direct_mail_publisher_test.cpp` (`direct_mail_publisher`) | `ctest --test-dir build -R '^direct_mail_publisher$'` |
| Presets summary projection | `tests/agent_preset_summary_test.cpp` (`agent_preset_summary`) | `ctest --test-dir build -R '^agent_preset_summary$'` |
| Global preset catalog projection | `tests/preset_catalog_test.cpp` (`preset_catalog`) | `ctest --test-dir build -R '^preset_catalog$'` |
| Sleep marker + observation | `tests/agent_sleep_test.cpp` (`agent_sleep`) | `ctest --test-dir build -R '^agent_sleep$'` |
| Desktop lifecycle matrix/state machine/process safety | `tests/agent_lifecycle_test.cpp` (`agent_lifecycle`) | `ctest --test-dir build -R '^agent_lifecycle$'` |
| No-TUI dispatch + stale UI delivery | `tests/native_shell_test.cpp` (`native_shell_lifecycle`) | `ctest --test-dir build -R '^native_shell_lifecycle$'` |
| Incremental Kanban snapshot/index | `tests/kanban_model_test.cpp` (`kanban_model`) | `ctest --test-dir build -R '^kanban_model$'` |
| Kanban SWR/single-flight generation | `tests/native_shell_test.cpp` (`native_shell_kanban`) | `ctest --test-dir build -R '^native_shell_kanban$'` |
| Repro-6 stale-observation rule | `tests/agent_sleep_test.cpp`, `tests/native_shell_test.cpp` | ctest names above |
| Repro-7 official checked/confirmed update lifecycle | `tests/test_desktop_user_cli.py` | `python3 -m unittest tests.test_desktop_user_cli` |

Visual fidelity against Telegram and functional fidelity against the TUI are
compared at review against the oracle behavior, not automated in this
repository's test suite.

Direct-conversation attachment cards render below their owning message in
projected order for both directions. Bounded valid images show thumbnails;
unsafe or undecodable images remain visible as ordinary file cards. Open and
Reveal refresh the current route and message entry, reopen no-follow, and
require matching regular-file device/inode/size before the shell's injected
external action. Failure uses the transient notice and preserves text,
chronology, scroll, and draft. History remains observation-only and ignores
outgoing send caps.
