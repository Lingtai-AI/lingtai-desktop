# `tests/` current proven behavior

This file records what the current `tests/` surface actually proves, grouped
by exact test/ctest owner and runnable command surface. It complements
[`tests/ANATOMY.md`](ANATOMY.md) (structure) and
[`tests/CONTRACT.md`](CONTRACT.md) (evidence rules) and the repository map
[`../ANATOMY.md`](../ANATOMY.md). Behavior here is current truth, not a
design target: a change to observable behavior belongs in the same change as
its code and its proof.

## Runnable command surface

Repository/package/lifecycle Python contracts complement the CMake/ctest
product-behavior proofs. The exact validation surface is in
[`../AGENTS.md`](../AGENTS.md):

```bash
python3 -m unittest tests.test_repository_contract
python3 -m unittest tests.test_app_archive
python3 -m unittest tests.test_desktop_user_cli
ctest --test-dir build --output-on-failure -R '^project_attachment$'
ctest --test-dir build --output-on-failure -R '^attachment_selection$'
ctest --test-dir build --output-on-failure -R '^agent_projection$'
ctest --test-dir build --output-on-failure -R '^direct_conversation_route$'
ctest --test-dir build --output-on-failure -R '^slash_command$'
ctest --test-dir build --output-on-failure -R '^direct_conversation_history$'
ctest --test-dir build --output-on-failure -R '^direct_mail_publisher$'
ctest --test-dir build --output-on-failure -R '^agent_preset_summary$'
ctest --test-dir build --output-on-failure -R '^preset_catalog$'
ctest --test-dir build --output-on-failure -R '^project_creation$'
ctest --test-dir build --output-on-failure -R '^agent_sleep$'
ctest --test-dir build --output-on-failure -R '^agent_lifecycle$'
ctest --test-dir build --output-on-failure -R '^kanban_model$'
ctest --test-dir build --output-on-failure -R '^posix_descriptor_primitives$'
ctest --test-dir build --output-on-failure -R '^workspace_selection$'
ctest --test-dir build --output-on-failure -R '^native_shell(_behavior)?$'
ctest --test-dir build --output-on-failure -R '^mac_popup_dismissal$'
./scripts/smoke.py
```

`ctest -R '^native_shell(_behavior)?$'` runs both the real-Qt shell behavior
and the process smoke; `./scripts/smoke.py` runs the same `--smoke` with the
Qt plugin path set and an 8 s watchdog.

## What each proof layer establishes

### Repository/build/static contracts

- `test_repository_contract.py` proves the canonical product version is
  v0.1.10, the lock file still pins Qt 6.11.1, the exact `tdesktop_commit`, and
  the seven exact toolkit commits, and that no dependency/build/validation
  artifact is tracked by git. It is the sole owner of pinned provenance
  (`test_repository_contract.py:2-8`).
- `test_app_archive.py` proves the portable archive/manifest producer and
  independent extractor/verifier remain the authoritative artifact boundary.
- `test_desktop_user_cli.py` proves entirely offline that bootstrap and explicit
  update select exact stable assets from the fixed official GitHub Release
  source, reject hostile metadata/redirects/statuses/stream sizes, clean partial
  downloads, bound every remote/managed version seam, and feed the existing
  installer. It also proves pre-transport URL encoding rejection, pre-update
  command syntax validation, the exact isolated staged `--smoke` invocation,
  its 60-second ceiling and fail-closed ordered markers, the owned private
  single-link cache and its
  hardlink-publication rollback, 24-hour zero-network freshness, normal-command
  noninteractive notices,
  interactive default-No offers, deliberate `y`/`yes` verified update followed
  by the original command, prompt-free explicit update/uninstall, failure-open
  availability, failure rollback, cache tamper refusal, and full uninstall under
  fake HOME plus injected transport/platform/clock/TTY/prompt boundaries.
- The compile-time guards prove the Qt-free consumers stay Qt-free (a
  `#ifdef QT_CORE_LIB`/`#error` in `workspace_selection_test.cpp:1-3`,
  `direct_conversation_route_test.cpp:1-3`, and
  `posix_descriptor_primitives_test.cpp:1-3`), and the `static_assert`s pin
  `noexcept` and exact return types of the C1 model and the containment/
  route seams (`workspace_selection_test.cpp:261-269`,
  `project_attachment_test.cpp:36`, `direct_conversation_route_test.cpp:99`).

### Pure/domain unit tests

- `project_creation` proves the exact human/Agent/shared-library shape,
  selected-preset default/allowed parity, reviewed setup fields, localized
  `en`/`zh`/`wen` `.prompt` plus empty/whitespace-only Comment adaptive
  playbooks, complete generated-placeholder substitution, byte-exact nonblank
  comment content,
  the final `manifest.comment_file`, publication
  with missing runtime/env/covenant inputs, staged validation, typed
  stage/detail survival through `ProjectCreationRunner`, normal attachment/
  setup compatibility, byte-preserving no-change save, preservation of
  pre-existing destination contents, conflicting `.lingtai` refusal, injected
  rollback after staging/generation/marker removal, publish-refusal cleanup,
  joined runner destruction with no late callback, and destination/preset
  symlink rejection. `test_project_creation_source_contract` separately proves
  production rollback contains no path-recursive `remove_all` call and no TUI
  or runtime-readiness adapter, and pins the Desktop-owned compiled content
  source into the creation target. `native_shell_new_window_bootstrap` also
  proves the create-new Comment widget starts empty, preserves leading/trailing
  whitespace through asynchronous creation, and keeps the accepted typed
  failure status visible.

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
- `attachment_selection` proves ordered regular-file acceptance with canonical
  source, display name, exact size and accepted-byte accounting; conservative
  case-insensitive image classification through its shared public pure owner;
  canonical duplicate, missing,
  directory, FIFO, and practical unreadable rejections; inclusive 25 MiB and
  cumulative 100 MiB boundaries; rejected-file budget behavior; and a
  type/size-identical fixture tree after preflight
  (`attachment_selection_test.cpp`).
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
  sent collapse preferring `sent`; text-only compatibility; current-entry
  rooting for inbox, sent, and pre-pickup outbox attachment metadata; exact
  opened sizes without outgoing limits; shared media classification; ordered
  duplicates; independent bad-attachment accounting with message/good-sibling
  survival; malicious serialized-parent containment; symlinked directory/file
  and non-regular rejection; no-write snapshots; one generic skipped count per
  bad neighbor with no valid neighbor hidden; and an intermediate symlink never
  exposing an outside mailbox. It also proves one shared scan classifies busy,
  sparse, and empty routes while retaining per-route dedupe/skip/attachment
  semantics, and pins the index's single-flight, stale project/route rejection,
  in-scan mutation follow-up, unchanged-tick, and reset/late-result behavior
  (`direct_conversation_history_test.cpp`).
- `direct_conversation_attachment_actions` proves success only for the freshly
  reopened current-entry regular file and rejects missing, replaced, symlinked,
  non-regular, escaping, or device/inode/size-mismatched requests before any
  external action (`direct_conversation_attachment_actions_test.cpp`).
- `direct_mail_publisher` proves the text-only envelope remains field-compatible
  and attachment-free; attachment-only and mixed publication use exact private
  copies, deterministic extension-preserving duplicate names, and final human
  sent paths while bytes remain in outbox before pickup. Queued outcomes expose
  the exact stamped row and copied outbox path/size/device/inode facts, while
  failures expose no published-row facts. It also pins source
  identity/type/size/access and forged-limit revalidation, structured failure
  facts, both-empty no-write, owned-leaf rollback (including a forced
  destination failure), fresh ids/nonoverwrite, and symlinked-route containment
  (`direct_mail_publisher_test.cpp`).
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
- `agent_lifecycle` proves the complete command matrix and Desktop-owned
  nonblocking protocol: descriptor/no-follow markers, held/free/stale advisory
  locks, exact argv process safety, TERM/KILL bounds, CPR launch outcomes,
  live/dead clear completion, default/allowed refresh presets, per-phase
  timeouts, and aggregate `all` results. `native_shell_lifecycle` additionally
  proves stale UI result suppression through the Desktop-owned controller.
- `kanban_model` proves complete cold-board semantics and deterministic
  incremental behavior: two unchanged generations open zero payloads and do
  not enumerate daemon runs; token/event/chat/delegate appends consume only
  complete appended rows; partial/reset sources remain generation-safe; a
  test hook moves all four growing sources and SQLite exactly after full-read
  completion, proving immediate affected-Agent repair, stable full-reader
  parity, incompatible-cursor ordering, and no human-row follow-up loop; a
  >1 MiB unterminated row proves capture incapability stays idle while its
  stamp is unchanged, then rebuilds exactly once to parity when completed; a
  300-run inventory reopens only new/nonterminal/replaced records; malformed,
  unsafe, newest-128, provider, recent, session, context, and tree facts retain
  full-reader parity; SQLite-only tool appends use one bounded query and a new
  molt boundary takes the rare repartition path (`kanban_model_test.cpp`).

### Real-Qt widget/shell contracts

- `mac_popup_dismissal` proves the macOS-only application-wide boundary with
  a real `QApplication`, Cocoa-hosted Desktop shells, and constructed
  `NSEvent`s. Its native traffic-light and panel controls receive their action
  exactly once after the popup is already hidden; same-window Qt content and
  a second shell receive one down/up pair without event consumption or focus
  theft. Root/submenu recipient windows publish zero requests, while outside
  and nil recipients publish one; left/right/other down qualify, but
  drag/up/move, no-popup, release-after-hide, and late repeated delivery do
  not. It also proves one bridge across two shells, exact tree hide counts,
  deferred deletion safety, existing Escape/focus ownership without the force
  stream, the pure identity table, and the direct-Qt native-boundary guard.
- `native_shell_behavior` proves the composed shell on a real `QApplication`
  with the real widgets shown off-screen: dark palette inheritance and
  restoration, live light→dark→light and dark→light→dark scheme transitions
  that inspect the composer's stored text/placeholder/selection colors and
  document format, its Send/attachment paint, and the actual Agent Config,
  Agent Presets, and Preset Editor control palettes/QSS, open-project behavior, shell
  semantics and named regions, selected-Agent conversation, composer send,
  Request sleep / Start Agent / Presets panels,
  Kanban stale-while-revalidate updating/failure states, Reload single-flight,
  and old-project generation rejection,
  first-project bootstrap, layout modes, the persistent roster shell, the
  dashboard layout, and the Telegram theme reset — all against synthetic
  `commit-N-...-fixture` trees under the CMake-created no-write fixture.
  Read-only, no-escape, and no-write cases use `project_tree` snapshots
  proving the snapshotted fixture (and any explicit outside symlink target
  the test names) remains unchanged; the send/sleep/bootstrap write journeys
  assert the exact intended in-fixture mutations
  (`verify_composer_send_behavior`, `verify_request_sleep_action`,
  `verify_first_project_bootstrap`).
- The focused `native_shell_status_item` journey directly triggers the owned
  menu without Accessibility or global menu-bar interaction. It proves one
  adapter across two shells without publishing a real offscreen tray item,
  exact Show/separator/Quit order, single callback delivery, deterministic and
  most-recent selection, hidden Show retaining `WA_DontShowOnScreen`, plain and
  maximized minimized restore, safe fallback to an owned shell after recent-
  shell removal, secondary-close preservation, a mask icon, exact transparent
  18/36-pixel zero resources, deterministic alpha-only nonzero masks pinned to
  that same 18/36-pixel height (never taller, naming and guarding the
  platform tray plugin's status-item pixmap cap directly) with a compact,
  formula-derived width containing the logo's own meaningful-alpha ink
  cropped and enlarged (never distorted) to fill most of the fixed canvas
  height, and a badge sized near that enlarged logo's own width (within a
  modest, documented tolerance) from a real, genuinely legible font -- never
  shrunk below a minimum readable pixel size to force a tighter width --
  deeply overlapping its lower-right region at dimensionless ratios
  converted from the accepted human visual reference (never measured
  against the nominal 18x18 box), with cross-scale consistency between
  1x/2x, a footprint-primacy and hard no-erasure bound keeping the logo
  perceptually primary, a non-vacuous real-cutout gate requiring a
  substantial strongly-cleared pixel area across multiple rows/columns (not
  a single low-alpha fringe pixel) for every displayed bucket at both
  scales, `99+` display capping, exact aggregate tooltips, and
  unchanged-bucket icon reuse. A dedicated regression additionally proves
  the declared badge/logo/canvas rectangles are invariant to
  `QApplication::setFont()` across several deliberately different real font
  families, so the declared geometry can never again silently depend on
  which real font family the running platform resolves an unstyled font to
  (the badge's own width/height are fixed constants; only the glyph itself
  may be condensed horizontally, never widened or shrunk in height, to stay
  inside the fixed envelope on every platform). The separate status-item
  Quit and final-window-close journeys each run a bounded Qt event loop and
  prove exit code 0 before their failure watchdogs can fire.
- The focused `native_shell_unread` journey uses two canonical Projects and
  duplicate real shells to prove one process-session total and coherent roster
  badges; background/minimized/hidden accrual; active visible catch-up from an
  already accepted snapshot; lifecycle-neutral valid-Agent membership; invalid
  route exclusion; duplicate and last-window close behavior; Project rebind;
  same-session reopen cursors; and synchronous final-close exclusion. It does
  not publish the offscreen status item or claim physical menu-bar layout. A
  held-worker suffix releases accepted mailbox work only after shell removal
  and host shutdown, then drains posted events to prove no stale unread callback
  survives either ownership boundary.
- The direct-conversation journey invokes the real ordinary one-second timer
  seam and observes actual `message.json` opens: an unchanged completed
  generation opens none; after an append the timer's UI thread opens none and
  one shared worker opens every fixture entry exactly once even with multiple
  valid Agent routes. Append-without-reselection, bottom-follow, scrolled-up
  position, and a Ctrl+U-expanded older-history window all remain intact. It
  also observes zero `QTextDocument::contentsChange` notifications on the idle
  tick and stable pre-existing frame/block identity across the real append.
- Focused history tests prove stable/unrelated per-Agent revisions, exact append
  ancestry, and rejection of replacement, prefix edit, attachment change,
  reorder, and shrink. Reaction/session tests prove real semantic revision
  advances and duplicate/downgrade/empty operations remain idempotent. The real
  conversation surface proves revision no-op, suffix-only frame retention,
  new-day bottom follow, scrolled anchor retention, Ctrl+U retention, and safe
  full replacement on a revision gap.
- `verify_composer_send_behavior` additionally proves the injected picker
  cancel path; ordered file/image cards; descriptor-revalidated bounded
  thumbnail fallback for replaced, symlinked, invalid, and absurd-header
  sources; second-picker duplicate and cumulative 100 MiB accounting over the
  existing draft; the 25 MiB limit; keyboard removal and narrow wrapping;
  attachment-only publication; slash-command isolation; target-switch
  clearing; a two-card second-source publication failure retaining text/cards
  and marking only index 1; and a general publisher failure retaining the
  draft with no false per-card error. Its event-loop-bounded replacement-timer
  assertion proves an older deadline cannot clear a newer composer notice.
- The focused `native_shell_paste` journey carries plain and HTML+plain source
  representations in real `QMimeData`, delivers them through real Qt
  drag/drop events to the `Ui::InputField` MIME insertion path, and asserts exact
  multiline Unicode/emoji/ZWJ reconstruction, rich-source logical fallback,
  selected-range replacement, caret/edit continuation, a distinct emoji IME
  commit, exact ordinary Send envelope/rendered row/draft clear, and two
  simultaneous shells sharing exactly one application-owned emoji runtime.
  It runs on Cocoa but never obtains or changes `QClipboard`.
- The focused `native_shell_menu` journey compares every recognized Qt
  standard action's order, shortcut, and enabled state against a fresh
  unhooked standard menu across empty, text, selection, Paste-derived, and
  undoable states. It asserts synchronous presentation of one `Ui::PopupMenu`,
  triggers Undo, Delete, and Select All against the real composer, and
  proves there are no spelling, Search Google, or rich-format actions. The
  unchanged synthetic same-window-click behavior remains a direct-Qt boundary
  guard for the existing native popup-dismissal bridge
  (`native_shell_test.cpp:3115`).
- `verify_existing_agent_setup` proves the selected-Agent `/setup` route on a
  synthetic project and hermetic `LINGTAI_TUI_DIR`: full saved/template catalog
  visibility/order, normalized real-row preselection without fallback, shared
  saved/template editing and materialization, unresolved additive fallback,
  reference-safe policy with unknown/active round-trip, 1/3→2/3→3/3 and Back
  routes, full review hydration, fixed identity/folder, byte-exact no-change
  and cancellation, owned-field save, selected-project refresh, typed
  source-change/rollback evidence, and both mode resets.
- `verify_first_project_bootstrap` uses a fake Desktop global catalog and a
  PATH with no TUI executable. It proves asynchronous single-pending discovery
  and creation with no runtime executable, normal attach/selection, setup-
  compatible output, successful injected first launch, and the recoverable
  created-but-not-started result after an injected launch refusal. The host-
  owned secondary-window journey additionally proves a typed draft-stage
  no-follow preset detail remains visibly rendered after queued delivery.
- `visual_composer_attachments_light` and `_dark` capture the selected-Agent
  detail with one file card and one decoded image thumbnail at the normal
  macOS viewport. Missing baselines preserve the actual PNG as an inspection
  artifact without implicitly accepting it.
- `visual_conversation_attachments_{light,dark}` and their narrow variants
  capture incoming image/file cards, outgoing corrupt-image fallback, ordered
  multiple attachments, filename elision, both actions, and responsive
  light/dark presentation. `WRITE_UI_ARTIFACTS=1` retains actual/diff evidence
  even for a passing comparison.
- The removed Activity and Task Card destinations are proven absent: their
  page-nav buttons, panel surfaces, headings, state lines, and section owners
  have no surviving widget anchors, the page navigation retains exactly
  Conversation + Presets, and the low-level
  `lingtai_selected_agent_status_activity` fact label stays present with its
  stable identity (`verify_removed_activity_and_task_card_destinations`).
- The dedicated `native_shell_presets` test proves the simplified Presets
  presentation: a selected Agent's Resolved Presets page renders exactly the
  minimal Provider/Model/Default/Allowed text with ordered refs, the removed
  active-ref/badge/context/capability/source-provenance text is absent, and
  the six generic fact widgets stay hidden on that page
  (`native_shell_presets_test.cpp`).
- In the selected-Agent conversation, one incoming mail fixture carries a real
  decoded JSON newline (U+000A) in its body; the one incoming multiline mail
  plus the one outgoing mail still occupy exactly two aligned message
  blocks/bubbles — the normalized line separator never splits a message into
  extra blocks — and the full literal multiline body stays selectable/copyable
  in `toPlainText`
  (`verify_selected_agent_conversation`,
  `tests/native_shell_test.cpp:908-1015`).

- `conversation_surface_typography` proves the dedicated widget/document
  typography contract on the real `ConversationSurface` for representative
  incoming and outgoing messages: the author/name renders as its own 15px
  DemiBold fragment, the body as 14px Normal, and the timestamp and subject
  as 13px Normal / 13px Medium metadata fragments, proving author >= body >
  metadata with the exact 15/14/13 pixel values.
- Its attachment section additionally proves incoming/outgoing order, multiple
  cards, narrow non-overlap, long-name tooltip/accessibility semantics, bounded
  valid image decode, corrupt-image file fallback, size/type labels, and two
  semantic action anchors per attachment. Real viewport pointer events prove
  that same-anchor Open/Reveal clicks emit their exact request once, while a
  text-selection drag ending over an action emits nothing and retains the
  native `QTextEdit` selection.
- `conversation_surface_scroll` sends real `QWheelEvent`s to the production
  viewport and proves that zero/tiny phase-bearing intent cancels delayed
  bottom pins before integer movement, append/rebuild yield through momentum,
  `ScrollEnd` does not jump, normal no-gesture bottom-follow and manual
  non-bottom retention remain intact, and ordinary wheel movement still
  traverses native `QTextEdit` handling.

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
  to a real Agent's `events.jsonl`/`mailbox`, or reads a real
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
- **Physical popup acceptance.** The focused Cocoa test authenticates native
  dispatch in its own process but does not replace later human testing of the
  packaged App: same-window background, a second window, native dialogs and
  titlebar controls, root/submenu inside clicks, Escape/deactivation, trackpad,
  a secondary display, and repeated fresh launch/open/click/quit cycles remain
  physical acceptance gates.
- **Real platform widgets' OS behavior beyond what the fixture exercises.**
  The real `Ui::RpWindow`/`Ui::RpWidget` composition is exercised, but not
  the OS window-server interaction a visible window would surface.
- **Literal system paste.** On this macOS Qt build, a full `NativeShell` under
  the `offscreen` or `minimal` platform crashes before the journey because
  `QRhiWidget` is unsupported. The automated paste journey therefore proves
  the shared MIME insertion/input-method formatting and Send behavior without
  accessing the host clipboard; literal Cmd-V from a real source remains an
  isolated candidate-App acceptance requirement.

## Standing evidence discipline

The standing UI-phase rule is one authentic RED plus one final directly
corresponding test per small commit; do not expect or promise per-commit
full-suite or sanitizer ceremony on this surface. Proof here is additive:
each ctest is the sole focused contract for its target
([`tests/ANATOMY.md`](ANATOMY.md)), and the evidence rules that govern
adding or removing any of it are in [`tests/CONTRACT.md`](CONTRACT.md).
