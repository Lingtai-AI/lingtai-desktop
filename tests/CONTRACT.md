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

- **Repository/static contracts** — `tests/test_repository_contract.py`,
  `tests/test_app_archive.py`, `tests/test_macos_packaging.py`,
  `tests/test_desktop_user_cli.py`, `tests/test_desktop_support_update.py`, and
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
   Clipboard-sensitive macOS tests must not write the Cocoa host clipboard.
   If a full shell cannot run with an offscreen platform clipboard, carry the
   MIME source deterministically and drive the real editor input-method path,
   then state literal system-paste acceptance as an unautomated gate.
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
- `tests/agent_setup_store_test.cpp` — `agent_setup_store`; bounded load,
  owned-field preservation, TUI preset/peer-orchestrator parity, external
  configured-env merge, and atomic rollback/no-mutation failure evidence.
- `tests/preset_catalog_test.cpp` — `preset_catalog`; injected global-root
  saved/template discovery, validation/skips, canonical ordering, exact facts,
  missing-directory behavior, typed scan failure, normalization, and no-write.
- `tests/project_creation_test.cpp` — `project_creation`; project inputs,
  destination contents, exact custom Comment bytes, fixture-derived bounded
  Desktop adaptations for all three languages with injected clock/location,
  rendered-output rejection of TUI-only prose and commands outside the public
  Desktop slash registry, controlled saved/template manifest projection,
  legacy/provider capability normalization, stable preset-policy de-duplication,
  project-root `.recipe`/`.tui-asset` absence, no-global-mutation snapshots,
  unsafe links, marker-present/marker-removed/
  publish-refused rollback, runner destruction, and expected output all live
  below injected fixture roots.
- `tests/test_project_creation_source_contract.py` — static assertion that the
  production creation owner uses descriptor primitives, contains no path
  recursive `remove_all` call, network/global guidance boundary, or impossible
  Desktop guidance, and that all seven pinned adaptive fixtures retain their
  recorded SHA-256 values.
- `tests/agent_sleep_test.cpp` — `agent_sleep`.
- `tests/agent_lifecycle_test.cpp` — `agent_lifecycle`; the complete
  Desktop-owned lifecycle component contract with injected process, launcher,
  and clock adapters.
- `tests/kanban_model_test.cpp` — `kanban_model`; complete-board parity plus
  deterministic payload/cursor/daemon incrementality counters and rebuild
  generation-race/capture-incapability liveness seams.
- `tests/native_shell_test.cpp` — split `native_shell_<journey>` tests (links the shell +
  `lib_ui` + `crl_integration.cpp`; the real-Qt layer), including the focused
  synthetic existing-Agent setup rerun, no-TUI New Project, literal bare/home-
  child and terminal-separator destinations committed through real destination
  editors and Review Create buttons, exact fake-HOME normalization, visible
  draft-preserving creation rejection/retry (without claiming native picker,
  literal system Command-V, or packaged-App submission),
  `native_shell_paste`, and `native_shell_lifecycle` journeys. Lifecycle UI
  coverage includes nonblocking delivery and stale-generation suppression. The
  paste journey delivers logical text through a real MIME-carrying Qt event,
  then proves exact Unicode editor state, emoji input-method formatting,
  ordinary Send, and two-shell runtime sharing without accessing `QClipboard`.
- `tests/conversation_surface_typography_test.cpp` —
  `conversation_surface_typography` (the dedicated widget/document typography
  contract on the real `ConversationSurface`).
- `tests/conversation_surface_scroll_test.cpp` —
  `conversation_surface_scroll` (the dedicated real-Qt viewport-wheel and
  gesture-aware bottom-follow contract on `ConversationSurface`, without the
  shell or composer lifecycle).
- `tests/test_native_shell.py` — `native_shell` (process persistence and
  smoke-order via the built smoke executable).
- `tests/test_repository_contract.py` — manual `python3 -m unittest
  tests.test_repository_contract`; the canonical product-version,
  pinned-provenance, and tracked-artifact repository contract.
- `tests/test_macos_packaging.py` — manual `python3 -m unittest
  tests.test_macos_packaging`; the offline package-production and independent
  per-slice verification boundary contract, including publication races and
  rollback for the optional DMG experiment.
- `tests/test_app_archive.py` — manual `python3 -m unittest
  tests.test_app_archive`; the primary portable-App producer and independent
  verifier contract, including exact App/manifest facts, executable modes,
  internal links, streaming digests, incremental hostile-member/resource
  rejection, bounded cleanup, and no-clobber publication races.
- `tests/test_desktop_user_cli.py` — manual `python3 -m unittest
  tests.test_desktop_user_cli`; the deterministic fake-HOME contract for fixed
  official GitHub App release discovery/download, cached confirmed offers, and
  the App archive transaction, plus canonical support manifest/state/pending
  models, final fresh-install generation layout, stable pre-import launcher, and
  local N-to-N+1 transaction. The support proof includes hostile file kinds,
  exact sets/modes/hashes/links, semantic last-good/high-water/failed hash closure,
  prepublication anti-rollback/failed-target/argv policy, exact-byte import, and
  enforced isolated no-write/network/process/native/frame/early-exit self-test.
  Its wrapper-owned terminal marker cannot be forged by inherited `os.write` or
  `posix.write`, candidate `__main__` replacement, partial/extra bytes, or descriptor
  close, while clean exact `True`, exception, timeout, and sticky denied-event
  dispositions remain explicit. Focused matrices cover ordinary and journaled
  fresh-install boundaries; payload/manifest/generation/current TOCTOU wedges;
  canonical absolute-launcher staging for bare, alternate, default, retry, and
  argument-looking argv0 forms; exact stable-launcher replay and executor/hash
  identity; command substitution rejection; mutation-independent
  last-good recovery; authenticated rollback at each durable boundary; exact
  one-shot explicit retry; numbered tag/version coherence; App receipt owner/type/
  link/mode/size/canonical-byte validation; post-current exact-pointer restoration;
  concurrent App-current replacement; support-independent App-only uninstall,
  doctor, and full support preflight. Injected transport/platform/
  clock/TTY/prompt boundaries keep every response and App action offline and
  deterministic; explicit App pairs and the injected local support transaction
  remain covered. App-specific legacy matrices use the explicit one-shot skip
  seam so their transport assertions stay scoped to the App plane.
- `tests/test_desktop_support_update.py` — manual `python3 -m unittest
  tests.test_desktop_support_update`; the dedicated fake-HOME/fake-transport
  Phase 3 owner. It proves latest/exact official support discovery, exact stable
  metadata/repository/tag/manifest/two-payload route and byte binding, hostile
  case/path/duplicate/redirect/JSON/length/SHA refusal, complete-validation-before-
  publication, independent exact cache/cadence/decline and restoration, including
  post-publication-read and post-final-check substitution on both prior-present and
  prior-absent branches. The selected threat contract calls this a cooperative private transaction namespace; arbitrary uncooperative same-UID replacement inside that namespace is outside the supported threat model. Canonical is the canonical arbitrary-racer boundary, and preserved canonical-racer diagnostics are not ordinary updater-owned residue.
  Tests bind prior bytes and identity to one descriptor read, prove post-`mkdtemp`
  chmod failure and both stage readback failures clean the private namespace, and
  ensure initially absent publication never depends on unlinking a flat
  support-root stage. The final exchange is the publication linearization point;
  its immediately following support-directory fsync is the transaction
  commit/durability boundary. Pre-commit fsync failure attempts exact prior-state
  recovery while preserving intervening racers, whereas post-commit cleanup fsync
  failure leaves the new canonical cache and emits one bounded committed
  diagnostic. Mutation after the exchange is later external canonical mutation
  for snapshot semantics even while failure before commit triggers recovery. A
  simultaneous primary error remains primary even when cleanup also fails. The
  tests bind the real final atomic exchange as the publication operation, prove
  that an exact retained prior inode is restored without
  deleting/overwriting the racer, prove that an initially absent-cache racer is
  atomically returned to canonical, and force canonical to disappear after
  restoration identity capture so bounded exclusive-link recovery restores the
  prior. They assert exact inode/bytes/mode/link counts, displaced-racer leaves,
  and complete staged-residue cleanup, all
  default-No/n/no/invalid/y/yes plus non-TTY/offline behavior, failed-target
  no-auto-retry, explicit support-before-App reporting, local-pair bypass,
  deterministic release production, and full official stage→pending→canonical
  reexec→bootstrap commit with App identity unchanged.
  Publication/refusal tests assert complete before/after trees, and support-only
  tests additionally retain App versions/receipts/current/bundle bytes and inode
  identities exactly. Evidence is valid only when shared-parent metadata,
  racer-owned leaves, outside trees, both managed planes, and the launcher remain
  byte/type/mode/identity identical except for the authorized transaction.

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
