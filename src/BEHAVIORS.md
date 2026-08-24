# `src/` current behavior

This file records the current observable behavior of the owned `src/` code as
it stands in this tree, with the TUI functional boundary (what the kernel/TUI
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
  reruns `apply_system_palette` (`native_shell.cpp:513`), which resets to
  the default light palette and only then applies the canonical night
  palette when the system prefers dark; the conversation is then
  re-rendered and the window and its descendant widgets repainted
  (`refresh_system_palette`, `native_shell.cpp:1215`). No fixed user theme
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
- The argument-free current surfaces are exact and case-sensitive: `/presets`
  selects the existing Presets page; `/agents` uses the existing narrow Back
  path or, in the wide two-column view, preserves selection and focuses its
  existing roster row; `/sleep` and `/cpr` reuse the existing selected-Agent
  Request sleep and Start owners and their status surfaces; `/help` reports only
  `/agents`, `/presets`, `/sleep`, `/cpr`, `/help`, and `/quit`; `/quit` closes
  only the Desktop window.
- Every other parsed command, including every argument-bearing form (such as
  `/sleep all` and `/cpr all`) and later lifecycle command, stays local, sets
  exactly `Command not available in this Desktop build.`, and performs no mail
  or Agent lifecycle side effect.

## State and selection

- `WorkspaceSelectionState` is the only active-project/Agent truth. A valid
  row click is the sole selection entry point (`handle_agent_selection`);
  only rows whose manifest is `valid` are selectable, and any other click
  shows `lingtai_agent_selection_error`.
- Selecting an Agent clears the composer and the pending sleep/start
  observations, resets to the conversation page,
  recomputes the layout, and focuses the composer when visible/enabled.
- The detail title is the manifest `nickname`, else `agent_name`, else the
  directory key; the subtitle shows the key (when it differs) plus
  `role: … · presence: …`.

## Open / refresh

- `open_project` attaches the selected directory, requires a real, safe,
  contained `.lingtai` directory, projects one `AgentSnapshot`, and activates
  C1 — preserving the selected Agent only when the canonical root is
  unchanged and the key is still valid. Failed opens keep the prior
  project/roster/selection and show a transient `lingtai_project_open_error`
  message.
- A one-second view-scoped `QTimer` (the only poller) re-invokes the same
  stateless readers for conversation and Presets, and
  keeps the Request-sleep / Start-Agent button states honest; it never
  carries a cursor or offset.
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

## New Project (TUI functional boundary)

- `New Project…` runs `<configured-tui> presets` headless (exact separate
  argv); with no configured executable it fails closed with a single status.
  While pending, both New/Open actions are disabled and the status shows the
  current phase.
- On success a small Desktop-owned dialog shows destination + Browse, a
  preset chooser populated from returned names (description/tier/source as
  help), and explicit `Create & Start` / `Cancel`. Any dismissal — Cancel,
  close control, or Escape — is the same no-spawn cancellation.
- `Create & Start` requires a nonempty destination and preset, then runs
  `<configured-tui> spawn <destination> --preset <name>` (no
  `--agent-name`/`--language`; current TUI defaults control them). Success
  attaches the returned `project_dir` only through `open_project` and reports
  `Project created and Agent started.`; failure/nonzero/malformed leaves the
  current project unchanged and states the destination may contain a
  partially initialized project. Desktop never writes project/Agent/config.
  Anchor: `ProjectBootstrapRunner` parse rules (`project_bootstrap.cpp:53`),
  shell handlers (`native_shell.cpp:1286-1399`).

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
  delivery. Empty text with no attachments writes nothing.

- The composer is one vendored single-line `InputField` plus an explicit
  `Send` `RoundButton`, enabled only when a direct route resolves for the
  current selection. The `Message…` placeholder hides as soon as the field
  has any text. Typing a leading `/` (with no following space) opens a
  focus-free slash-command popup of Desktop commands; an exact unique name
  or ordinary text dismisses it. Arrow keys move the highlight, Tab/Enter
  insert the selected command, and Escape closes the popup.
- After raw slash classification returns no command, ordinary send re-resolves
  the route fresh (never a stale captured target), trims the text, rejects
  whitespace-only input without writing, and calls `send_direct_mail`.
  Outcome text is exactly `Queued` on success or `Message was not queued.`;
  the ordinary-message composer clears only on success. The conversation state
  line shows `N message(s)` plus ` · N skipped` when the one generic skipped
  count is nonzero.
- The conversation surface renders rows read-only as plain text (kernel
  `message` field), chronological by the kernel's timestamp precedence with
  the entry ID as tie-break; markup is never interpreted. This is the TUI
  functional boundary: delivery, replies, and unread state are neither read
  nor inferred.
- `read_direct_conversation` also projects read-only attachment metadata without
  changing that text presentation: it ignores every serialized parent, accepts
  only a safe final basename opened beneath the current inbox/sent/outbox
  entry's own real `attachments` directory, measures the opened regular file by
  `fstat`, preserves valid JSON order and duplicates, and reads no content.
  Missing/malformed/unsafe/symlinked/non-regular attachment entries increment
  `skipped_attachments`, never `skipped`, so the message and good siblings
  survive. These local paths are observations only; open/reveal must revalidate.

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

## Request sleep

- Empty-form exact case-sensitive `/sleep` clears the composer status and calls
  only the existing selected-Agent Request sleep handler, reusing its
  `request_agent_sleep` owner, eligibility, observation, and status surface.
  The Request sleep moon control is not shown in the selected-Agent header.
- Eligibility at invocation time: valid manifest, main/agent role, `alive`
  presence, and a known `.agent.json.state` other than `asleep`/`suspended`.
- The shared handler reruns `project_agents` once, captures a log baseline,
  writes the `.sleep` marker, shows exactly `Sleep requested.`, disables the
  button, and
  observes for at most 3 s via the existing timer. Terminal text reports
  whether `sleep_received(source="signal_file")` was observed and the
  re-projected current state — never `queued` or a lifecycle verdict from the
  write/timeout alone. Anchor: `tests/agent_sleep_test.cpp` /
  `agent_sleep` ctest.

## Start Agent

- Empty-form exact case-sensitive `/cpr` clears the composer status and calls
  only the existing selected-Agent Start handler, reusing its `start_agent`
  owner, eligibility, observation, and status surface.
- The Start action is not shown in the selected-Agent header. Empty-form
  `/cpr` is the product affordance; the hidden Start owner still enables
  for a valid main/agent row with a stale or missing heartbeat so slash
  dispatch can reuse it. If the shared handler's fresh re-read instead finds the
  selected row heartbeat-live, it reports exactly `Agent is already online.`;
  missing, nonselected, and other ineligible cases keep their existing behavior.
- The shared handler reruns `project_agents` once, then starts
  `<python> -m lingtai run <agent-dir>` detached and shell-free, redirecting
  stdout/stderr to the Agent's own `logs/agent.log` (created first). It shows
  `Starting Agent...` and observes for at most 10 s: success is proven only
  by the projection later reporting this exact selection `alive`
  (`Agent is online.`), otherwise `Agent did not come online. See
  <agent>/logs/agent.log.`. A local refusal shows `Could not start Agent. See
  <agent>/logs/agent.log.` immediately. Anchor:
  `tests/native_shell_test.cpp` (`native_shell_behavior` ctest) exercises the
  shell composition that hosts this control.

## Test anchors

All contract tests are listed with their exact ctest names in
[`src/CONTRACT.md`](CONTRACT.md) and the root [`../ANATOMY.md`](../ANATOMY.md);
the process-level smoke-order and persistence contract is
`tests/test_native_shell.py` (`native_shell` ctest), and the shell semantics
(no project writes, geometry, named regions) are `tests/native_shell_test.cpp`
(`native_shell_behavior` ctest, `QT_QPA_PLATFORM=cocoa` on macOS else
offscreen).
