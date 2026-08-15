# `tests/` current proven behavior

This file records what the current `tests/` surface actually proves, grouped
by exact test/ctest owner and runnable command surface. It complements
[`tests/ANATOMY.md`](ANATOMY.md) (structure) and
[`tests/CONTRACT.md`](CONTRACT.md) (evidence rules) and the repository map
[`../ANATOMY.md`](../ANATOMY.md). Behavior here is current truth, not a
design target: a change to observable behavior belongs in the same change as
its code and its proof.

## Runnable command surface

All repository-owned automated product-behavior proof is CMake/ctest-driven;
the repository contract is one manual Python command. The exact validation
surface is in
[`../AGENTS.md`](../AGENTS.md):

```bash
python3 -m unittest tests.test_repository_contract
ctest --test-dir build --output-on-failure -R '^project_attachment$'
ctest --test-dir build --output-on-failure -R '^agent_projection$'
ctest --test-dir build --output-on-failure -R '^direct_conversation_route$'
ctest --test-dir build --output-on-failure -R '^direct_conversation_history$'
ctest --test-dir build --output-on-failure -R '^direct_mail_publisher$'
ctest --test-dir build --output-on-failure -R '^agent_activity$'
ctest --test-dir build --output-on-failure -R '^agent_task_card$'
ctest --test-dir build --output-on-failure -R '^agent_preset_summary$'
ctest --test-dir build --output-on-failure -R '^agent_sleep$'
ctest --test-dir build --output-on-failure -R '^posix_descriptor_primitives$'
ctest --test-dir build --output-on-failure -R '^workspace_selection$'
ctest --test-dir build --output-on-failure -R '^native_shell(_behavior)?$'
./scripts/smoke.py
```

`ctest -R '^native_shell(_behavior)?$'` runs both the real-Qt shell behavior
and the process smoke; `./scripts/smoke.py` runs the same `--smoke` with the
Qt plugin path set and an 8 s watchdog.

## What each proof layer establishes

### Repository/build/static contracts

- `test_repository_contract.py` proves the lock file still pins Qt 6.11.1,
  the exact `tdesktop_commit`, and the seven exact toolkit commits, and that
  no dependency/build/validation artifact is tracked by git. It is the sole
  owner of pinned provenance (`test_repository_contract.py:2-8`).
- The compile-time guards prove the Qt-free consumers stay Qt-free (a
  `#ifdef QT_CORE_LIB`/`#error` in `workspace_selection_test.cpp:1-3`,
  `direct_conversation_route_test.cpp:1-3`, and
  `posix_descriptor_primitives_test.cpp:1-3`), and the `static_assert`s pin
  `noexcept` and exact return types of the C1 model and the containment/
  route seams (`workspace_selection_test.cpp:261-269`,
  `project_attachment_test.cpp:36`, `direct_conversation_route_test.cpp:99`).

### Pure/domain unit tests

- `posix_descriptor_primitives` proves the descriptor seam: exactly-once
  ownership with a real recycled-descriptor double-close probe
  (`posix_descriptor_primitives_test.cpp:47-103`), read-only/`O_CLOEXEC`/
  `O_NOFOLLOW`/`O_NONBLOCK` flags verified on real opens and `fcntl`s, a
  FIFO proving nonblocking behavior (`:107-161`), directory-stream adoption
  closing only the adopted descriptor (`:164-190`), and `safe_leaf` rejecting
  every unsafe leaf shape (`:192-204`).
- `project_attachment` proves the containment seam: canonical-root retention,
  typed failures (`selection_not_found`, `selection_not_directory`,
  `filesystem_error` with retained `ENOTDIR`/`EACCES`), rejection of
  absolute/empty/dot/`..`/escaping paths, and that no resolution ever writes
  to the tree (`project_attachment_test.cpp:69-199`). The permission fixture
  is honestly skipped when run as root or on non-POSIX platforms
  (`:94-128`).
- `workspace_selection` proves the C1 model: closed start, activation owning
  the canonical root, safe/unsafe key validation, same-root preservation
  versus different-root clearing, idempotent clear, and that deleted roots
  and every transition leave the tree byte-identical (`workspace_selection_test.cpp:93-256`).
- `agent_projection` proves the one composite projection: coarse
  `unavailable` vs distinct complete-empty scans, sorted membership with an
  unsafe child hidden but never hiding healthy siblings, the exact
  role matrix, the 1 MiB manifest boundary (at-limit valid, over-limit
  malformed), the heartbeat presence matrix with a non-negative exposed age,
  the `.status.json` matrix with zero/wrong-typed-window context suppression,
  capability intrinsics ordering and dedupe, and byte-identical reads
  (`agent_projection_test.cpp:103-434`).
- `direct_conversation_route` proves the pure route: one selected valid
  non-human row plus exactly one valid human resolves with current
  human/target keys and addresses, canonical root and target `agent_id`
  anchors, and the exact human sender card; every other case — absent/
  inexact selection, malformed/unsafe rows, no identity facts, human as its
  own target, two humans, shared addresses — fails closed with no typed
  failure evidence retained, and no path is ever touched
  (`direct_conversation_route_test.cpp:95-184`).
- `direct_conversation_history` proves the read-only mailbox projection:
  incoming+outgoing render from `message` (never the misleading `body`) in
  timestamp order; exact envelope membership (scalar `to`, mismatched
  incoming `agent_id` absent, another conversation's mail absent); outbox/
  sent collapse preferring `sent`; one generic skipped count per bad
  neighbor with no valid neighbor hidden; and an intermediate symlink never
  exposing an outside mailbox (`direct_conversation_history_test.cpp:113-264`).
- `direct_mail_publisher` proves the one exclusive outbox leaf: fresh
  `YYYYMMDDTHHMMSS-xxxx` id, exactly `message.json` with the current schema
  fields, pre-existing content untouched, a fresh id per send, and a blocked
  or symlinked outbox failing closed with no outside write
  (`direct_mail_publisher_test.cpp:91-207`).
- `agent_activity` proves the bounded suffix projection: exact selected-Agent
  binding, file-order allowlist (public diary + completed tool_call/tool_result
  only), partial-tail completion on the next stateless snapshot, the 512 KiB
  actual-read bound ignoring a larger prefix, a 100-row cap, and malformed
  complete rows counted once without hiding valid neighbors
  (`agent_activity_test.cpp:120-303`).
- `agent_task_card` proves exact `active`/`inactive`/`unavailable`
  projection bound to the selected key, nonblank UTF-8 body unchanged on
  active, no body on inactive, and a symlinked `taskcard` component reducing
  to unavailable with no outside body and no write
  (`agent_task_card_test.cpp:77-125`).
- `agent_preset_summary` proves `resolved` (exact ordered allowed refs with
  independent active/default badges, narrow active-effective fields,
  optional `context_limit` omission still resolved), `stale` (supported
  artifact strictly older than the selected Agent's own `init.json` mtime),
  and `unavailable` (symlinked `system` component, no partial projection),
  all with no write (`agent_preset_summary_test.cpp:99-221`).
- `agent_sleep` proves the exact-target `.sleep` write: marker lands only on
  the selected key (sibling byte-identical), overwrite truncates to zero
  bytes, a missing key fails without creating its directory, a pre-baseline
  or partial-tail-completed `sleep_received` is never attributed while a
  post-baseline complete row is, and symlinked/non-regular leaves or
  intermediate directories fail closed with nothing written outside
  (`agent_sleep_test.cpp:90-233`).

### Real-Qt `native_shell_behavior`

- `native_shell_behavior` proves the composed shell on a real `QApplication`
  with the real widgets shown off-screen: dark palette inheritance and
  restoration, live light/dark scheme transitions sampling the current
  representative palette tokens/assertions, open-project behavior, shell
  semantics and named regions, selected-Agent conversation, composer send,
  Agent Activity / Request sleep / Start Agent / Task Card / Presets panels,
  first-project bootstrap, layout modes, the persistent roster shell, the
  dashboard layout, and the Telegram theme reset — all against synthetic
  `commit-N-...-fixture` trees under the CMake-created no-write fixture.
  Read-only, no-escape, and no-write cases use `project_tree` snapshots
  proving the snapshotted fixture (and any explicit outside symlink target
  the test names) remains unchanged; the send/sleep/bootstrap write journeys
  assert the exact intended in-fixture mutations
  (`verify_composer_send_behavior`, `verify_request_sleep_action`,
  `verify_first_project_bootstrap`).

### Process-level smoke/persistence

- `native_shell` proves the built executable's process behavior: `--smoke`
  prints exactly one `LINGTAI_NATIVE_SHELL_READY` followed by exactly one
  `LINGTAI_LIB_UI_FULL_TARGET_SMOKE_OK` carrying the class/window/body/
  sidebar/content/separator/roster/empty/offscreen/shown markers in order,
  and exits 0; `--offscreen` proves normal mode does not exit on its own
  (`test_native_shell.py:24-82`).

## What synthetic/offscreen tests do not establish

Every test here runs on fixture trees, a fake display, or a subprocess with
the real shell shown off-screen. None of them establish:

- **Real terminal bytes.** No test starts a real `lingtai run` kernel, writes
  to a real Agent's `events.jsonl`/`mailbox`/`taskcard`, or reads a real
  agent's actual bytes. The `agent_sleep` observation and the
  `project_agents` heartbeat are proven against fixture files with pinned
  timestamps, never against a live kernel. Inside `native_shell_behavior`
  the Start Agent path does assert the exact separate `-m`, `lingtai`,
  `run`, absolute-Agent-directory argv and that a fixture process's
  captured stdout+stderr land in the selected Agent's `logs/agent.log`;
  what it does not prove is real kernel/provider semantics (a real Agent
  coming online, real model/provider interaction).
- **Human-visible acceptance.** `native_shell_behavior` and the smoke run on
  the real shell but off-screen (`show_offscreen()`, `WA_DontShowOnScreen`
  on Cocoa, Qt offscreen elsewhere). No test renders to a visible display or
  asserts what a human sees; pixel-level or perceptual acceptance is not
  proven here. The palette/theme assertions drive live light and dark scheme
  transitions through the real-Qt journey and sample exactly the current
  representative palette tokens and states (dark palette inheritance, the
  live light/dark scheme transition, the theme-reset check); they do not
  prove complete visible Telegram parity or every painted pixel/token.
- **Real platform widgets' OS behavior beyond what the fixture exercises.**
  The real `Ui::RpWindow`/`Ui::RpWidget` composition is exercised, but not
  the OS window-server interaction a visible window would surface.

## Standing evidence discipline

The standing UI-phase rule is one authentic RED plus one final directly
corresponding test per small commit; do not expect or promise per-commit
full-suite or sanitizer ceremony on this surface. Proof here is additive:
each ctest is the sole focused contract for its target
([`tests/ANATOMY.md`](ANATOMY.md)), and the evidence rules that govern
adding or removing any of it are in [`tests/CONTRACT.md`](CONTRACT.md).
