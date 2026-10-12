# `tests/` contract

This file governs the remaining automated test surface. Exact target/command
registration is in [`ANATOMY.md`](ANATOMY.md), `CMakeLists.txt`, and
[`../AGENTS.md`](../AGENTS.md). Production contracts in
[`../src/CONTRACT.md`](../src/CONTRACT.md) are unchanged by test retirement.

The remaining wire tests use actual filesystem operations under disposable
injected roots. The process smoke launches the built App, not a fake UI.
The repository gate protects pinned inputs and tracked-artifact hygiene.
The separately invoked `agent_lifecycle_real_smoke.cpp` is real-kernel manual
acceptance: it requires explicit operator authorization, an existing configured
runtime, and disposable HOME/project paths. It must not become an unattended
ctest or a claim of provider/UI acceptance.

## Evidence rules

1. **Authentic semantic RED.** New behavior is proven by writing a real,
   failing, semantic assertion first: the test asserts the observable
   contract (a projected row, a written leaf, a refused path), fails
   authentically against current code, and only then is the implementation
   changed to make it pass. A test that never failed is not evidence; a test
   that fails only because of a fixture bug or a crash is not a RED.
2. **One final, directly corresponding test per small commit.** Every small
   commit that changes behavior lands with exactly one final test that
   corresponds directly to the behavior it introduced — no orphan test
   written ahead of code that never shipped, no behavior change shipped with
   only a passing test from a different slice. Ted's standing UI-phase rule
   is exactly this: one authentic RED plus one final directly corresponding
   test. Do not promise per-commit full-suite or sanitizer ceremony; the
   focused test is the ceremony.
3. **Exact candidate bind (selected-Agent/project tests).** A test of
   selected-Agent or project behavior must bind to the exact selected
   candidate (`agent-a`, the `worker-dir` key, the precise human address)
   and prove that a sibling, an outside tree, or a wrong key produces no
   row, no write, and no read-through. This rule governs that family of
   tests, not every test in `tests/`: pure-seam tests (descriptor
   primitives, containment) bind to their own exact fixture shapes instead.
4. **Fixtures owned under temporary/injected roots.** Every domain test takes
   its sandbox from ctest, rebuilds a synthetic tree inside it, and removes
   it at the end. Never point a test at a real `.lingtai` project, a real
   Agent, a home-directory path, or any developer-owned directory. The
   process smoke uses the built App executable; wire tests receive injected
   fixture roots. Neither is proof of an OS-level sandbox.
   Clipboard-sensitive macOS tests must not write the Cocoa host clipboard.
   If a full shell cannot run with an offscreen platform clipboard, carry the
   MIME source deterministically and drive the real editor input-method path,
   then state literal system-paste acceptance as an unautomated gate.
   A native-control test may construct only test-process windows and may
   temporarily replace a test window control's target/action with a harmless
   counter; it must restore the original target/action before teardown and
   must never operate the installed LingTai App or another process.
5. **No real Agent/project mutation in unattended tests.** A ctest must never start a real kernel,
   open a real Agent's mailbox, sleep a real Agent, or mutate a real
   registry/settings file. All reads and writes happen on fixture trees built
   and torn down by the test itself, and the exact byte/type image
   (`tree_snapshot`) is compared to prove the snapshotted fixture and any
   explicit outside symlink target it names remain unchanged. Expected
   writes stay under the injected fixture trees by dependency/path
   injection; the snapshot is not an OS/process sandbox and makes no claim
   about arbitrary unobserved outside paths.
6. **No network/provider dependence in unattended tests.** A ctest must pass offline, with no
   provider, model, API key, or live service. Every source is a fixture file;
   the only "live" fact allowed is wall-clock time where the production
   contract itself is time-relative (heartbeat liveness, sleep observation),
   and even there the fixtures are pinned clearly on either side of the
   threshold with margins.
7. **No redundant matrix/implementation-shape assertions.** Do not assert the
   implementation's shape (internal field names, parse order, helper
   signatures) as if it were a contract, and do not add a second test that
   re-proves a boundary an existing test already proves. The former role,
   presence, and status matrices are retired, not a template for rebuilding
   parallel suites; a matrix that cannot catch a realistic bug is redundant. A test that asserts only
   implementation shape will break on a legitimate refactor and should be
   deleted, not fixed.
8. **Delete tests that cannot catch a realistic important bug.** Any test
   whose removal would not permit a realistic, important bug to pass is dead
   weight. Keep a test only if there is a concrete, plausible regression it
   alone would catch (a symlink escaping containment, a sibling's mail
   appearing in the conversation, a sleep marker landing on the wrong
   target). Prefer deleting such a test to maintaining it; prefer not adding
   it in the first place.

## Current registration and retirement

Only `direct_conversation_history`, `direct_mail_publisher`, and `native_shell`
are registered with ctest. The repository gate and real-kernel lifecycle target
are listed in `ANATOMY.md`. A production library need not retain a separate
implementation-mirroring suite merely to fill an inventory row.

Retirement deliberately gives up broad domain/failure matrices, hidden-widget
and synthetic-shell interactions, visual snapshots, native popup event tables,
static-source shape assertions, and injected installer/package transactions.
Do not claim those proofs from the remaining smoke tests. Existing `BEHAVIORS.md`
files contain historical proof anchors, not current registration or new
validation receipts.

## Maintenance

Keep registration, fixture argv, labels, and commands aligned with CMake.
Retained tests must continue to fail loudly on failed semantic assertions.
New behavior still needs authentic focused evidence; retiring low-signal tests
is not a blanket claim that tests are pointless. No mock may stand in for a
required native/manual acceptance result.
