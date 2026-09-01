# `src/` current behavior

This file records the current observable behavior of the owned `src/` code as
it stands in this tree, with the kernel functional boundary
headless surface owns) and the Telegram visual-oracle boundary (which visual
facts are inherited from the pinned `lib_ui` palette) stated for each surface.
It complements [`src/ANATOMY.md`](ANATOMY.md) (structure) and
[`src/CONTRACT.md`](CONTRACT.md) (ownership rules) and the repository
[`../ANATOMY.md`](../ANATOMY.md). Behavior here is the current truth, not a
design target; a change to observable behavior belongs in the same change as
its code.

## Window and startup

- Normal execution constructs one `NativeShell`, shows it, and schedules no
  automatic exit; `--offscreen` and `--smoke` are the only special argv
  (`main.cpp:15`).
- Application composition initializes the pinned toolkit's emoji runtime once,
  after widget styles and before the first composer is constructed. Every
  simultaneous or later `NativeShell` under that `QApplication` reuses it;
  the application-owned guard clears emoji state only during `QApplication`
  teardown, after stack-owned `ShellHost`/`NativeShell` widgets are destroyed.
- On macOS, when Desktop supplies `Ui::Integration`, application composition
  also installs one native event filter shared by every shell. For left,
  right, or other mouse-down, the filter collects existing visible top-level
  `Qt::Popup` Cocoa windows without creating native resources. A recipient in
  that set is inside and remains entirely under existing popup behavior; any
  other recipient, including no recipient window, synchronously publishes one
  force-hide request. The filter always returns `false`, so the original
  Cocoa/Qt target receives the unchanged event once after popup subscribers
  have hidden their trees. With no visible popup it publishes nothing.
- All visible top-level `Qt::Popup` windows are conservatively classified as
  inside. Re-audit this policy if Desktop or the pinned toolkit introduces a
  non-menu top-level `Qt::Popup`; there is intentionally no speculative
  coordinate fallback.
- The window is named `lingtai_desktop_window`, body `lingtai_desktop_body`,
  with a persistent left sidebar and a right content pane separated by a
  one-pixel `Ui::PlainShadow` (`lingtai_roster_separator`).
- Smoke mode shows the real shell off-screen, verifies readiness and named
  regions, prints `LINGTAI_NATIVE_SHELL_READY` then
  `LINGTAI_LIB_UI_FULL_TARGET_SMOKE_OK` (with the class/window/body/sidebar/
  content/separator/roster/empty markers) in that order, and exits; a 3 s
  watchdog prints `LINGTAI_LIB_UI_SMOKE_TIMEOUT` and exits 99.
  Anchor: `tests/test_native_shell.py` (order of the two markers, empty route
  visible, persistence of normal mode), `main.cpp:55`.

## Theme

- The palette is started once, before the window is built: if the system
  prefers dark (Qt `colorScheme()` dark/light, with a palette-lightness
  fallback, `system_prefers_dark_palette`, `native_shell.cpp:448`), the
  shell applies Telegram's canonical night palette
  (`apply_telegram_night_palette`, `native_shell.cpp:457`); otherwise the
  default light palette is used. This honors the system appearance at
  startup.
- The same system appearance is followed live: `QStyleHints`
  `colorSchemeChanged` (with an `ApplicationPaletteChange` event fallback)
  reruns `apply_system_palette` (`native_shell.cpp:1105`), which resets to
  the default light palette and only then applies the canonical night
  palette when the system prefers dark, then publishes that completed
  `lib_ui` palette transaction exactly once. Palette subscribers therefore
  rewrite stored inner control colors only after every token is final.
  The shell also asks Agent Config, Agent Presets, and Preset Editor to
  reapply their page-owned literal QSS/QPalette chrome; the conversation is
  then re-rendered and the window and its descendant widgets repainted
  (`refresh_system_palette`, `native_shell.cpp:2567`). No fixed user theme
  or config is mutated — the active palette is always re-derived from the
  current system appearance.
- The Telegram visual-oracle boundary: every painted token (list field,
  hover, selected rows, bubbles, button states, separators) comes from the
  shared `lib_ui` palette (`st::windowBgOver`, `st::dialogsBgActive`,
  `st::msgInBg`, `st::msgOutBg`, `st::defaultLightButton`, etc.), never a raw
  white/black Qt surface.

## Conversation command classification

- `parse_slash_command` classifies only raw text whose first byte is `/` and
  which has at least one following byte. It splits once on the first literal
  ASCII space, trims the argument edges, preserves unknown names and internal
  argument spacing, and owns no dispatch or side effect. A bare slash, leading
  whitespace, empty text, and ordinary text are not commands.
- `NativeShell::handle_send_message` calls that classifier on the raw composer
  text before the ordinary trim/mail path. Every parsed command clears the
  composer and returns through local dispatch, so unknown, unavailable, and
  argument-bearing commands never reach `send_direct_mail`.
- The navigation surfaces are exact and case-sensitive: `/presets`
  selects the existing Presets page; `/agents` uses the existing narrow Back
  path or, in the wide two-column view, preserves selection and focuses its
  existing roster row. `/sleep`, `/suspend`, `/cpr`, `/clear`, and `/refresh`
  dispatch through the Desktop lifecycle controller; `/help` names them and
  `/quit` closes only the Desktop window.
- Lifecycle arguments follow the exact matrix: `all` is accepted for sleep,
  suspend, CPR, and refresh; clear is single-target; a named refresh preset is
  single-target. Invalid forms stay local and perform no lifecycle side effect.

## State and selection

- `WorkspaceSelectionState` is the only active-project/Agent truth. A valid
  row click is the sole selection entry point (`handle_agent_selection`);
  only rows whose manifest is `valid` are selectable, and any other click
  shows `lingtai_agent_selection_error`.
- Selecting an Agent clears the composer and the pending sleep/start
  attachment draft/errors, and pending sleep/start observations, resets to the conversation page,
  recomputes the layout, and focuses the composer when visible/enabled.
- The detail title is the manifest `nickname`, else `agent_name`, else the
  directory key; the subtitle shows the key (when it differs) plus
  `role: … · presence: …`.

## Kanban freshness

- Project open starts one low-priority cold Kanban generation. Once complete,
  `/kanban` paints that in-session board immediately and schedules one
  coalesced refresh without clearing it. Warm work displays `Updating…`;
  failure retains the last complete board and labels it stale.
- The ten-tick warm path calls the session index, not the legacy full reader.
  An unchanged project opens no ledger/history/daemon payload and performs no
  daemon-run enumeration. Complete JSONL appends advance token, event,
  history, and delegate cursors; partial trailing rows wait for their newline.
  `log.sqlite`/WAL metadata gates bounded session/event queries, with a rare
  affected-Agent token repartition only when the latest molt boundary changes.
- Cold, forced, and affected-Agent rebuilds compare growing-source and
  SQLite/WAL stamps from before the full read with the captured generation.
  Movement makes that Agent non-authoritative and requests one immediate
  affected-Agent repair; a paused writer converges without skipped or
  duplicated rows. A cursor that is capture-incapable without source movement
  waits for its stamp to change instead of chaining same-state rebuilds; that
  change then triggers one affected-Agent rebuild. Human rows do not
  participate in these growing-source follow-up checks.
- Daemon membership is enumerated only after the directory fingerprint
  changes. Completed summaries are immutable in-session; new, replaced, or
  nonterminal changed records alone are reopened. Consumers receive only an
  atomically composed complete `KanbanBoard`.
- Reload is single-flight even under repeated clicks. Project/Agent/source
  changes during a worker coalesce to one later generation, and a stale
  project generation cannot publish under the new C1 project truth.

## Open / refresh

- An unchanged selected conversation is revision-gated before receipt-history
  scans, full message/reaction/session equality, detail-state mutation, or
  `QTextDocument` mutation. A worker-proven direct append from the currently
  presented revision keeps the complete indexed history, copies only the new
  presentation suffix, retains existing message frames and the Ctrl+U window,
  and follows the bottom only when the viewport was already there and no
  trackpad gesture/momentum is active. Any viewport wheel event cancels an
  already queued delayed bottom pin before native `QTextEdit` handling; phase-
  bearing gestures retain reader ownership through momentum until `ScrollEnd`,
  even when a zero/tiny delta does not change the integer scrollbar value.
  `ScrollEnd` itself never jumps. Revision gaps, replacement/reorder/shrink, or
  independent presentation changes use a complete rebuild under the same
  gesture-aware follow rule.

- `open_project` attaches the selected directory, requires a real, safe,
  contained `.lingtai` directory, projects one `AgentSnapshot`, and activates
  C1 — preserving the selected Agent only when the canonical root is
  unchanged and the key is still valid. Failed opens keep the prior
  project/roster/selection and show a transient `lingtai_project_open_error`
  message.
- A one-second view-scoped `QTimer` (the only poller) performs a fixed-count,
  descriptor-relative fingerprint of the human mailbox and its
  `inbox`/`sent`/`outbox` leaves, re-invokes the Presets reader, and keeps the
  Request-sleep / Start-Agent button states honest. An unchanged mailbox
  opens no `message.json`. A changed fingerprint schedules at most one shared
  worker generation; that worker opens and parses each mailbox entry once,
  classifies it across all current Agent routes, and posts only a stable,
  current-project/current-route-set result to the UI thread. A mutation during
  the scan schedules one follow-up generation; stale results are discarded.
  The most recent completed same-route snapshot remains renderable while its
  replacement is in flight, and project/route switches never reuse it.
- After `send_direct_mail` atomically succeeds, its exact local-publication
  facts enter a project/Agent-keyed session projection before the composer is
  cleared. The ordinary conversation renderer merges those rows after the
  last accepted history, so rapid sends appear once in publication order
  without waiting for a mailbox scan. A snapshot that does not yet contain an
  ID leaves its pending row in place; the first accepted snapshot containing
  that ID retires the pending copy and presents the authoritative metadata
  without duplication. Project-open/invalidation clears the projection;
  selection changes cannot expose another route's rows.
- Roster state label: `Roster unavailable` when the scan is not complete,
  `No Agents found — scan complete`, or `N Agent(s) — scan complete`.

## Layout

- Telegram's two-mode derive is reproduced from the body's width: at or above
  `260 + 380` usable pixels (after the roster's one semantic 8px drag handle
  `lingtai_roster_resize_handle`, distinct from the one-pixel `Ui::PlainShadow`
  separator that follows it) the roster + handle + separator + detail all
  show, and the list expands from its 260 px absolute minimum toward a stored
  runtime-only ratio clamped to 22%-30% of the body while the detail keeps its
  380 px absolute minimum. A real drag on the handle re-derives that ratio, so
  a subsequent resize in the wide view keeps the chosen proportion; the ratio
  is never persisted. Below the threshold exactly one full-width surface shows
  — roster until an Agent is selected, then detail with a Back control
  (`recompute_layout`), and the handle is hidden. Back is the narrow
  history-back path: it drops the selection and returns to the roster,
  unchanged.
- The right content pane is one coherent workspace: the no-project welcome
  branding (title/purpose) and its rhythm spacing belong to the empty route
  only, so a selected project's active route begins at the content origin with
  no shared spacer left above the workspace.
- The selected-Agent chat top bar is responsive against its actual derived
  detail width: at each recompute the full natural top-bar row with the
  current key text is measured against that width (the body minus the actual
  chosen roster width, 8px handle, and 1px separator in Normal mode; the body
  width in OneColumn detail), and the secondary `lingtai_selected_agent_key`
  label hides first when it does not fit, returning as soon as sufficient
  width restores the fit. The presentation name, Back/Start/Request-sleep
  controls, fonts, and object names are never altered.

## New Project (Desktop-owned boundary)

- `New Project…` asynchronously calls `load_preset_catalog` against the
  Desktop global root. While discovery or creation is pending, duplicate
  New/Open activation is suppressed and the status shows the current phase.
- The existing setup wizard owns destination, saved/template selection,
  editor, allowed/default policy, reviewed language/configuration, and explicit
  Create/Cancel. Create mode starts at a `500000` context limit; an explicit
  edit is transported exactly, while existing-Agent setup loads the stored
  value unchanged. Any dismissal remains a no-create cancellation.
- Immediately before dispatch, the shell removes only terminal `/` separators
  from a non-root destination, then maps exact `~` and `~/...` text through a
  nonempty traversal-free absolute `HOME`. It preserves all-separator root text
  and every other byte; it does no shell, environment-variable, user-home, glob,
  dot, or traversal normalization. Missing/invalid `HOME` and shorthand
  traversal remain in the wizard as visible draft-validation errors without
  starting the transaction. `~user` and every other relative path remain
  literal and are rejected by the unchanged strict creation owner.
- `Create` validates the destination/draft/reference shape and reviewed
  selected preset before building one marker-owned sibling stage. It writes the
  canonical minimum human, first Agent, mailbox, and shared-library tree,
  validates allowed presets and reconciles selected/default/allowed policy
  inside staging. Fresh `manifest` starts only from the selected preset's `llm`
  and normalized `capabilities`: legacy `bash` becomes `shell`, unequal
  `bash`/`shell` definitions fail closed, and a nonempty LLM `api_key_env`
  replaces that field only on same-provider capability objects. Creation then
  adds its owned fields, so unrelated saved/template manifest keys do not
  leak. Requested allowed references retain first-seen order, duplicates are
  removed, and the absolute selected reference is appended only when absent.
  It then writes a Desktop-owned localized `.prompt` and always
  writes Agent-local `comment.md`. Empty or whitespace-only create-new Comment
  text selects a bounded Desktop-owned adaptation of the pinned adaptive
  recipe: product wording names Desktop, slash commands are limited to the
  Desktop registry, navigation names Ctrl+O or Cmd+O, add-on verification is
  platform-neutral, and the localized TUI full-command dumps are omitted.
  TUI-only commands, shortcuts, and asset paths cannot reach the rendered
  guidance. Local time is formatted `YYYY-MM-DD HH:MM`, location uses an
  injected already-cached value or `unknown`, and no location network lookup
  or global-state write occurs.
  Every nonblank text is transported and written byte-for-byte. The final `manifest.comment_file`
  always names that published Agent-local file. Validation requires exact
  Agent children, bounded regular
  content, resolved generated placeholders, matching content/references, and
  exactly one orchestrator.
  It syncs every directory before removing the marker and exclusively renames
  to `.lingtai`. Conflicts and pre-commit failures preserve existing state and
  remove only the exact open stage through descriptor-relative recursion;
  symlinked inputs fail closed. Runtime Python, `.env`, and covenant existence
  are launch concerns, not pre-stage publication gates.
- New Project disables Soul flow and cadence with `/setup` guidance because
  creation does not mutate shared runtime Soul-flow state. Existing-Agent
  `/setup` keeps the enabled toggle and its existing save semantics.
- After publication, the shell attaches only through `open_project` and starts
  the first Agent through `AgentLifecycleController`. Launch failure reports a
  recoverable created-but-not-started project; it never rolls back committed
  user-visible state. Pre-publication creation failures return to the same
  Review page with destination, preset, and configuration draft preserved;
  their stable typed stage and safe detail are visible there and retained
  through asynchronous UI delivery and diagnostic logging. No TUI executable
  is discovered or invoked.
  Anchor: `create_project` (`project_creation.cpp`) and shell handoff
  (`native_shell.cpp`).
- The kernel-consumed per-Agent `.prompt`/`comment.md` effects are shared
  on-disk behavior and therefore Desktop-owned here. `.tui-asset`, project
  `.recipe`, recipe snapshots/reconciliation, TUI config/Register/global
  utilities, preset credential persistence, and the TUI phantom-process
  recheck are intentionally nonapplicable; creation neither emits nor calls
  them.

## Existing Agent setup rerun

- `/setup` requires the active project's exact selected Agent and reuses the
  in-window setup wizard in an explicit existing-Agent mode. It loads one
  `AgentSetupState` through `AgentSetupStore` plus every valid saved/template
  row through Desktop's injected-root catalog loader, starts at Preset 1 of 3,
  and preselects the normalized real default/active row. Only an unresolved
  ref adds a selected Current setup fallback; the saved/template sections stay
  visible. Real rows use the shared editor before reference-aware Agent policy;
  fallback Continue bypasses the editor. Agent name/folder remain read-only.
- Existing policy stores full refs separately from friendly catalog display
  names. Editing a real row proposes its materialized saved ref, adds that ref
  to allowed, and preserves loaded unknown allowed/default/active refs and
  active semantics. Agents Back returns to Preset; Review Back returns to
  Agents.
- Save calls `AgentSetupStore::save` directly. `saved` and `no_change` close
  the route, refresh the selected project, and preserve selection; every typed
  failure stays open with its detail. Cancel, Back, and Escape write nothing.
  Entering New Project resets the same wizard to its creation semantics, and
  vice versa.

## Composer and conversation

- `preflight_attachments` is the UI-independent direct-file selection model:
  each selected path is canonicalized, opened nonblocking, and measured from
  the opened regular file; accepted metadata retains that canonical source,
  the selected leaf display name, exact bytes, device/inode identity, and a
  conservative case-insensitive image/file classification. Equivalent
  filesystem sources are rejected after their first occurrence. The inclusive
  25 MiB per-file and 100 MiB cumulative limits are applied in input order;
  rejection consumes no cumulative budget, so a later smaller file can still
  fit.
- `classify_attachment_media_kind` is the one shared, filesystem-free owner of
  the case-insensitive `.gif`/`.heic`/`.jpeg`/`.jpg`/`.png`/`.webp` filename
  classification used by both selection and history projection.
- Missing, non-regular, unreadable, oversized, over-total, duplicate, and other
  local failures remain distinct typed rejections carrying the rejected input.
  Preflight performs no write or copy, catches failures at its public boundary,
  and does not authorize publication.
- `send_direct_mail` keeps the two-argument text-only call and exact envelope
  (no `attachments` field). With accepted attachments it permits empty text,
  reopens each canonical source read-only/no-follow, checks regular-file
  device/inode identity and current size, reapplies per-file/cumulative limits,
  and copies from that same validated descriptor. Safe duplicate basenames use
  `stem-1.ext`, `stem-2.ext`, and so on. Private copies live temporarily under
  the owned outbox leaf, while the JSON array names their final absolute human
  `sent/<id>/attachments/<name>` paths.
- The publisher closes every copy before atomically publishing `message.json`
  last. Any source, name, limit, destination, copy, payload, or publish failure
  returns a typed reason (plus attachment index/source when applicable), never
  returns a message id, and removes only that call's exclusive leaf. `queued`
  means the complete outbox leaf exists; it does not mean kernel pickup or
  delivery. A queued result also returns the exact stamped id/timestamp/body
  and copied outbox file name/path/size/device/inode/media facts established by
  that publication; a failure returns none of them. Empty text with no
  attachments writes nothing.

- The composer is one vendored bounded multiline `InputField`, a 40 px
  paperclip, and an explicit `Send` `RoundButton`. The input and paperclip are
  enabled only when a direct route resolves; Send additionally requires
  trimmed text or at least one pending attachment. The `Message…` placeholder hides as soon as the field
  has any text. Typing a leading `/` (with no following space) opens a
  focus-free slash-command popup of Desktop commands; an exact unique name
  or ordinary text dismisses it. Arrow keys move the highlight, Tab/Enter
  insert the selected command, and Escape closes the popup.
- The composer accepts ordinary and multiline Unicode, recognized emoji and
  emoji sequences, and input-method commits through lib_ui's shared logical-
  text insertion path. A rich source carrying HTML plus plain text contributes
  that logical plain text only: the editor does not accept rich text and Send
  adds no HTML/formatting semantics. Inserted content replaces the active
  selection, leaves an editable caret after it, and `getLastText()` reconstructs
  the exact Unicode rather than exposing internal object-replacement markers.
- A composer context request retains Qt's standard Undo, Redo, Cut, Copy,
  optional Copy Link Location, Paste, Delete, and Select All actions in their
  Qt order, with their original shortcuts and enabled state. Pinned lib_ui
  constructs the one styled `Ui::PopupMenu` synchronously. Desktop adds no
  spelling, Search Google, Formatting, or other rich-message rows and performs
  no asynchronous platform work for the request.
- The paperclip asks `NativeShell` for the normal native multi-file dialog;
  tests replace that one picker seam. Each selection re-preflights existing
  canonical sources plus new picker paths once, preserving accepted order and
  applying duplicate and cumulative limits across the whole draft. Cancellation
  changes nothing. Route loss or project/Agent change clears the draft.
- Pending cards form a left-aligned wrapping row above the input. A thumbnail
  reopens the accepted source without following links, requires the same
  regular-file device/inode/size facts, and rejects implausible dimensions or
  allocation before a bounded decode. Changed, linked, invalid, or unsupported
  images use the normal file-type fallback without changing publisher
  authorization. Cards preserve the full filename when it fits, plus a
  full-path tooltip, human size, and keyboard remove.
  Selection feedback uses one replaceable 4.5-second semantic notice; typed
  send failures can additionally mark the indexed card until retry/removal.
- After raw slash classification returns no command, ordinary send re-resolves
  the route fresh (never a stale captured target), trims the text, and calls
  `send_direct_mail` with the current ordered draft. Attachment-only sends are
  valid. Slash commands do not publish, consume, clear, or mark attachments.
  Queued success clears text/cards/errors, refreshes history/receipt state, and
  scrolls as before; route/publisher failure retains the draft and shows a
  concise mapped notice. Pasted or input-method text uses this same ordinary
  Send path with no paste-specific branch. The conversation state
  line shows `N message(s)` plus ` · N skipped` when the one generic skipped
  count is nonzero.
- The conversation surface renders rows read-only as plain text (kernel
  `message` field), chronological by the kernel's timestamp precedence with
  the entry ID as tie-break; markup is never interpreted. This is the TUI
  functional boundary: delivery, replies, and unread state are neither read
  nor inferred. Its render-time tail window retains every older slice already
  revealed when the same conversation grows, so an unchanged or append tick
  never collapses that view back to the initial tail.
- `read_direct_conversation` also projects read-only attachment metadata without
  changing that text presentation: it ignores every serialized parent, accepts
  only a safe final basename opened beneath the current inbox/sent/outbox
  entry's own real `attachments` directory, measures the opened regular file by
  `fstat`, preserves valid JSON order and duplicates, and reads no content.
  Missing/malformed/unsafe/symlinked/non-regular attachment entries increment
  `skipped_attachments`, never `skipped`, so the message and good siblings
  survive. These local paths are observations only.
- Every projected attachment renders directly below its owning incoming or
  outgoing message in metadata order. Safely decoded images use bounded
  aspect-preserving thumbnails; corrupt, unsupported, changed, unsafe, or
  implausibly large images fall back to the same ordinary file card. File
  cards show an elided basename, full basename tooltip/accessibility text,
  human-readable size, stable type treatment, and `Open` / `Reveal in Finder`
  links. Existing message text and skipped counts remain visible regardless of
  attachment eligibility, and narrow layouts keep cards inside their message
  lane without overlap.
- Open/Reveal re-resolves the selected route and current message entry, then
  reopens the selected basename beneath that entry's current `attachments/`
  directory no-follow. Name/order and device/inode/size must still match the
  rendered observation. Missing, moved-away, replaced, symlinked, non-regular,
  escaping, or mismatched files invoke no external action. The shell owns one
  injectable external-action seam; production uses the macOS open/Finder
  mechanisms and tests use fakes only. Revalidation or invocation failure uses
  the existing transient composer notice without changing history, chronology,
  read/delivery state, scroll position, or draft.
- History attachment rendering is observation only: it does not reuse outgoing
  publisher authorization, apply the 25 MiB/100 MiB send caps, rescan arbitrary
  parents, persist new absolute paths, publish mail, or infer delivery/unread.

## Dashboard sections

- Exactly one selected-Agent page shows at a time: Conversation (default) or
  Presets. The Presets tab is not shown; `/presets` still opens that page and
  reveals the Conversation control as the way back. The chat-first detail
  hides the page-tab strip entirely. The duplicate
  `lingtai_selected_agent_conversation_heading` is retained only as a hidden
  object/implementation anchor.
- Presets (`read_agent_preset_summary`): reads the kernel-published
  `system/manifest.resolved.json` v1 envelope and shows only the minimal
  Provider, Model, Default, and ordered Allowed refs, with `Resolved`/`Not
  yet published`/`Stale`/`Unavailable` state. Every observation is shown
  exactly as read (no last-valid preservation).

## Agent lifecycle

- Every command refreshes the Agent snapshot at dispatch, resolves the selected
  non-human Agent or Main fallback, and returns immediately. A short controller
  timer advances explicit deadlines; no sleep/wait runs on the UI thread.
- Sleep distinguishes marker write from kernel application. Suspend waits for
  both heartbeat and exact process death. CPR refuses duplicate-live evidence,
  waits for the real advisory lease, launches directly, and requires a fresh
  heartbeat. Dead clear temporarily revives, observes molt/event completion,
  then suspends. Refresh validates the preset before suspend, proves lease
  release, escalates only exact matching processes, cleans stale handshakes,
  atomically updates active preset, relaunches, and verifies a fresh heartbeat.
- `all` targets run serially and return one aggregate retaining every failure's
  Agent and phase. Results carry project/selection generation; late results are
  suppressed after any selection transition. Lifecycle never invokes TUI.
  Anchors: `agent_lifecycle`, `native_shell_lifecycle`.

## Test anchors

All contract tests are listed with their exact ctest names in
[`src/CONTRACT.md`](CONTRACT.md) and the root [`../ANATOMY.md`](../ANATOMY.md);
the process-level smoke-order and persistence contract is
`tests/test_native_shell.py` (`native_shell` ctest), and the shell semantics
(no project writes, geometry, named regions) are `tests/native_shell_test.cpp`
(`native_shell_behavior` ctest, `QT_QPA_PLATFORM=cocoa` on macOS else
offscreen). The macOS native recipient, synchronous hide/click-through, event
matrix, submenu/deletion, and one-bridge behavior is anchored separately by
`tests/mac_popup_dismissal_bridge_test.mm` (`mac_popup_dismissal` ctest).
