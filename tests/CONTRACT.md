# `tests/` contract

This is the contract for the owned `tests/` surface: the normative evidence
rules an agent must follow before adding, changing, or deleting any test
here. It complements the repository map
[`../ANATOMY.md`](../ANATOMY.md) (structure), the structural inventory
[`tests/ANATOMY.md`](ANATOMY.md), the current proven behavior
[`tests/BEHAVIORS.md`](BEHAVIORS.md), and the source ownership rules
[`../src/CONTRACT.md`](../src/CONTRACT.md). Anatomy answers what runs here;
this file answers what evidence is worth keeping and how new behavior must
prove itself.

## Two kinds of contract

`tests/` holds two kinds of proof with two different lifecycles, and they
must never be confused:

- **Repository/static contracts** — `tests/test_repository_contract.py` and
  the compile-time guards and `static_assert`s inside the unit tests. These
  pin provenance, enforce build edges (a Qt-free consumer stays Qt-free), and
  pin `noexcept`/exact return types. They protect the shape of the build and
  the API, not user behavior; they change only when the pinned inputs or the
  public API change.
- **User behavior** — everything the domain unit tests, the real-Qt
  `native_shell_behavior`, and the process smoke actually exercise: what a
  reader projects, what a writer writes, what the shell composes and shows.
  A behavioral change to the product must land with its corresponding proof
  in the same change; a repository contract does not follow that rule.
  `slash_command_test.cpp` is the pure classification proof: it pins the raw
  leading-slash and first-ASCII-space boundary without asserting later dispatch.

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
   The `direct_conversation_route_test` deliberately uses distinct fixture
   vocabulary (`operator-1`/`worker-dir`, distinct addresses and `agent_id`s)
   so that identity can never be assumed.
   (`direct_conversation_route_test.cpp:41-47`.)
4. **Fixtures owned under temporary/injected roots.** Every domain test takes
   its sandbox from ctest, rebuilds a synthetic tree inside it, and removes
   it at the end. Never point a test at a real `.lingtai` project, a real
   Agent, a home-directory path, or any developer-owned directory. The
   Qt-aware tests use the CMake-created `native-shell-no-write-fixture` as
   their working directory and the smoke executable as a subprocess
   argument; both are injected roots, never real user data.
5. **No real Agent/project mutation.** A test must never start a real kernel,
   open a real Agent's mailbox, sleep a real Agent, or mutate a real
   registry/settings file. All reads and writes happen on fixture trees built
   and torn down by the test itself, and the exact byte/type image
   (`tree_snapshot`) is compared to prove the snapshotted fixture and any
   explicit outside symlink target it names remain unchanged. Expected
   writes stay under the injected fixture trees by dependency/path
   injection; the snapshot is not an OS/process sandbox and makes no claim
   about arbitrary unobserved outside paths.
6. **No network/provider dependence.** A test must pass offline, with no
   provider, model, API key, or live service. Every source is a fixture file;
   the only "live" fact allowed is wall-clock time where the production
   contract itself is time-relative (heartbeat liveness, sleep observation),
   and even there the fixtures are pinned clearly on either side of the
   threshold with margins (`agent_projection_test.cpp:267-273`).
7. **No redundant matrix/implementation-shape assertions.** Do not assert the
   implementation's shape (internal field names, parse order, helper
   signatures) as if it were a contract, and do not add a second test that
   re-proves a boundary an existing test already proves. One row matrix per
   projection (`test_role_matrix`, `test_presence_matrix`,
   `test_status_matrix`) is the retained ceiling; a further matrix that would
   not catch a realistic bug is redundant. A test that asserts only
   implementation shape will break on a legitimate refactor and should be
   deleted, not fixed.
8. **Delete tests that cannot catch a realistic important bug.** Any test
   whose removal would not permit a realistic, important bug to pass is dead
   weight. Keep a test only if there is a concrete, plausible regression it
   alone would catch (a symlink escaping containment, a sibling's mail
   appearing in the conversation, a sleep marker landing on the wrong
   target). Prefer deleting such a test to maintaining it; prefer not adding
   it in the first place.

## Contract tests

Each owned library target has exactly one focused CMake/ctest contract; the
target names, fixtures, and `-Wall -Wextra -Werror -pedantic` flags are in
[`tests/ANATOMY.md`](ANATOMY.md) and `CMakeLists.txt`:

- `tests/posix_descriptor_primitives_test.cpp` — `posix_descriptor_primitives`.
- `tests/project_attachment_test.cpp` — `project_attachment`.
- `tests/attachment_selection_test.cpp` — `attachment_selection`.
- `tests/workspace_selection_test.cpp` — `workspace_selection`.
- `tests/agent_projection_test.cpp` — `agent_projection`.
- `tests/direct_conversation_route_test.cpp` — `direct_conversation_route`.
- `tests/direct_conversation_history_test.cpp` — `direct_conversation_history`.
- `tests/direct_conversation_attachment_actions_test.cpp` —
  `direct_conversation_attachment_actions`; fresh current-entry-relative
  no-follow identity revalidation and fail-closed mutation matrix.
- `tests/direct_mail_publisher_test.cpp` — `direct_mail_publisher`.
- `tests/agent_preset_summary_test.cpp` — `agent_preset_summary`.
- `tests/agent_sleep_test.cpp` — `agent_sleep`.
- `tests/native_shell_test.cpp` — `native_shell_behavior` (links the shell +
  `lib_ui` + `crl_integration.cpp`; the real-Qt layer).
- `tests/conversation_surface_typography_test.cpp` —
  `conversation_surface_typography` (the dedicated widget/document typography
  contract on the real `ConversationSurface`).
- `tests/test_native_shell.py` — `native_shell` (process persistence and
  smoke-order via the built smoke executable).
- `tests/test_repository_contract.py` — manual `python3 -m unittest
  tests.test_repository_contract`; the sole repository/static contract.

## Maintenance

- Keep the ctest names, the fixture paths, and this inventory aligned with
  `CMakeLists.txt`; a test whose ctest name or fixture moved out of sync is a
  contract violation even if it still passes.
- A new owned behavior must land as: one authentic RED, the implementation,
  and one final directly corresponding test bound to its exact selected
  candidate (for selected-Agent/project behavior) under an injected fixture
  root — matching the standing UI-phase rule. No test may be added that
  duplicates an existing proof layer or that would not catch a realistic
  important bug.
- Repository/static contracts change only with the pinned inputs or the
  public API they guard; user-behavior tests change with the behavior they
  prove, in the same commit.
- The contract is normative: if a test and this contract disagree about what
  is provable (e.g. a test claiming real terminal bytes), treat the test as
  defective unless an authorized change updates both together.
