# `src/ui/` — observed behaviors

Current observable behavior only, for `AgentRoster` and `ConversationSurface`,
with exact source anchors. These describe what the source does today; they are
not a repair plan and make no forward claims.

## AgentRoster

### Roster status label

`update_state_label` (`agent_roster.cpp:302-311`) counts the visible rows —
the snapshot with the human pseudo-agent omitted (`visible_rows`,
`agent_roster.cpp:33-41`) — and sets exactly one of:

- `Roster unavailable` when `snapshot.scan != AgentScanState::complete`;
- `No Agents found — scan complete` when the scan is complete and no visible
  row remains;
- `N Agent(s) — scan complete` otherwise, with `N` the visible row count.

### Row rebuild vs. checked-state-only refresh

`set_rows` (`agent_roster.cpp:325-396`) renders only the visible rows: it
derives the presentation set by omitting the human pseudo-agent
(`visible_rows`, `agent_roster.cpp:33-41`) and compares the incoming visible
set against the previously shown visible set field by field
(`directory_key`, `manifest_kind`, `role`, `presence`,
`manifest_diagnostic`) with equal size (`agent_roster.cpp:337-351`). Two
outcomes:

- **Unchanged model:** only `update_checked_states(selected_key)` runs
  (`agent_roster.cpp:352-355`). The row tree, scroll position, focus, and row
  identity are preserved across the shell's one-second refresh; a human-only
  projection change never churns the real rows.
- **Changed model:** the row tree is torn down and rebuilt in snapshot order
  with the human omitted (`agent_roster.cpp:361-391`), then a stretch is
  appended.

### Row composition and visual states

Each row is an `AgentRowButton`, a checkable `QPushButton` whose intrinsic
preferred height is the larger of the fixed 40px avatar (`kAvatarDiameter`) and
the 15pt primary + 13pt secondary line metrics, plus the 8px vertical frame
top/bottom (`sizeHint`, `agent_roster.cpp:131-142`); its size policy is
expanding/preferred (`agent_roster.cpp:119-123`, `433`), with `Qt::StrongFocus`
(`agent_roster.cpp:122`). Its text is two lines, `key\nmanifest — role —
presence` (`agent_roster.cpp:421`, `98-102`), where the presence value is the
raw projection kind (`alive_human`/`alive`/`stale`/
`missing`/`invalid`/`unavailable`/`unknown`, `agent_roster.cpp:68-79`). The
accessible description appends `manifest diagnostic: …` when the row carries a
nonempty diagnostic (`agent_roster.cpp:104-110`, `422`).

Painting (`agent_roster.cpp:144-230`):

- Fill: selected → `st::windowBgOver` (calm neutral); down or hovered →
  `st::windowBgRipple`; otherwise `st::windowBgOver`.
- Selection cue: a checked row paints a narrow leading `st::dialogsBgActive`
  accent strip (`kSelectedAccentWidth`, 4px) at the row's left edge
  (`agent_roster.cpp:153-157`).
- A fixed leading circular avatar/initial: the 40px circle is filled from the
  name text color, and the first visible initial — the leading character of
  the first line with its doubled `&&` folded back to `&`, trimmed and
  upper-cased — is drawn centered in 15pt DemiBold with the actual neutral row
  surface token (`st::windowBgRipple` hover, `st::windowBgOver` otherwise) as
  pen.
- The text column begins at the avatar's right edge plus 1px and the 10px
  `kAvatarTextGap`, then the remaining row width, split into a top half
  (`primary_rect`) and bottom half (`secondary_rect`). Primary identity (15pt
  DemiBold, `&&`→`&` visible) renders at a purposefully larger visual scale
  than the secondary metadata line (13pt normal), which keeps its readable
  mature 13pt scale. On the neutral selected surface both pick the normal/hover
  readable `st::dialogsNameFg`/`st::dialogsTextFg` families
  (`st::dialogsNameFgOver`/`st::dialogsTextFgOver` on hover) rather than
  active-white text; both lines are right-elided with
  `QFontMetrics(font).elidedText(..., Qt::ElideRight, width)` at paint time and
  drawn without mnemonic processing. The row's full `text()`,
  accessibleName/Description, and properties are unchanged.
- A `PE_FrameFocusRect` is painted when the row has focus.
- The row palette `Highlight` is set to `st::dialogsBgActive` so the selection
  accent resolves from the same token its paint uses
  (`agent_roster.cpp:437-439`).

Enabled/checked/keyboard (`agent_roster.cpp:377-395`, `184-193`):

- A row is enabled only when its already-projected `manifest_kind == valid`;
  malformed/unsafe rows are visible but disabled.
- The row is checked when the caller's `selected_key` equals its `directory_key`.
- Return/Enter on a focused enabled row calls `click()`, which flows into the
  clicked → selection handler exactly like a mouse click.

### Click and focus routing

- A clicked enabled row forwards its `directory_key` through the one custom
  `RowClickHandler` (`agent_roster.cpp:388-392`); no other custom callback is
  emitted. The Open/New Project child `QPushButton`s expose their standard Qt
  `clicked` signals, which the owner composes but does not connect
  (`agent_roster.cpp:236-252`); `NativeShell` finds them by object name and
  wires them (`native_shell.cpp:593-603`).
- `focus_row(key)` focuses the first enabled row whose `directory_key` equals
  `key`, or the first enabled row when no key is given; disabled rows are
  skipped (`agent_roster.cpp:398-414`).

## ConversationSurface

### Content replacement and no-op refresh

- `set_plain_state(text)` is a no-op if the text already matches
  `last_plain_state_`; otherwise it clears any conversation and centers the
  plain text (`conversation_surface.cpp:113-127`).
- `set_conversation(them, messages)` is a no-op when `them` matches and
  `same_content` finds equal size and equal `id`/`outgoing`/`timestamp`/
  `subject`/`text` per row (`conversation_surface.cpp:129-145`, `147-157`).
  Otherwise the document is rebuilt, preserving scroll as below.

### Text and bubble layout

`rebuild_document` (`conversation_surface.cpp:180-237`) writes one
`QTextBlock` per message in the caller's order:

- Alignment left (incoming) or right (outgoing) inside a centered shared
  reading column (maximum 900px). At the explicit narrow breakpoint (viewport
  below 480px) each block is near-full: the viewport minus the two 12px edge
  gutters. Otherwise `message_block_width` applies the ordinary 72% ratio to
  the column with the 160px lower bound and the absolute readable cap of 640px,
  never wider than the column. One outer gutter (the centered-column offset
  plus the 12px edge) and one inner remainder are derived and cross-assigned in
  `message_block_format` so incoming stays left-anchored and outgoing
  right-anchored inside the same column rather than centering each message
  individually, with 4px/18px top/bottom margins
  (`conversation_surface.cpp:44-72`).
- Header line `You · <timestamp>` for outgoing, `<them> · <timestamp>` for
  incoming: the author/name is its own 15px DemiBold fragment in the out/in
  text color, then the literal ` · <timestamp>` at 13px Normal in
  `st::msgServiceFg` (`conversation_surface.cpp:188-195`, `53-62`, `64-72`).
- Optional subject line at 13px Medium using `st::historyTextOutFg` /
  `st::historyTextInFg` (`conversation_surface.cpp:196-200`, `74-83`).
- Body at 14px Normal in the same out/in text colors, inserted literally — the
  surface never interprets markup — after normalizing every paragraph
  delimiter (CRLF/CR, LF and U+2029) to a U+2028 line separator, so a decoded
  newline stays inside the one message block and never splits a message into
  extra blocks/bubbles while the stored text is untouched
  (`conversation_surface.cpp:210`, `85-94`).

`paintEvent` (`conversation_surface.cpp:218-270`) fills the viewport with the
`st::windowBgOver` chat backdrop, then paints an 8px-radius rounded bubble
behind each message block (`st::msgOutBg` outgoing, `st::msgInBg` incoming),
offset by the current scroll so bubbles stay aligned with text; finally
`QTextEdit::paintEvent` draws the text with native selection/copy behavior.

### Scroll and resize

- Before a rebuild, the surface records whether the vertical scrollbar sat at
  its bottom; after the rebuild it snaps to the new bottom only if the human
  was already there, otherwise it clamps the prior value
  (`conversation_surface.cpp:164-166`, `213-215`).
- `resizeEvent` reflows only when the quantized viewport/layout width
  (`int(width / 8) × 8`) changes; a live pixel-by-pixel resize does not rebuild.
  It follows the full layout width rather than the capped message width because
  a wide pane's centered column outer gutters keep moving after the message cap
  stops (`conversation_surface.cpp:293-307`).
