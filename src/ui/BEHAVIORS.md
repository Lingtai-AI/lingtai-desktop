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

Each row is an `AgentRowButton`, a checkable `QPushButton` fixed at 62px height
with `Qt::StrongFocus` (`agent_roster.cpp:114-125`). Its text is two lines,
`key\nmanifest — role — presence` (`agent_roster.cpp:374`, `95-99`), where the
presence value is the raw projection kind (`alive_human`/`alive`/`stale`/
`missing`/`invalid`/`unavailable`/`unknown`, `agent_roster.cpp:67-78`). The
accessible description appends `manifest diagnostic: …` when the row carries a
nonempty diagnostic (`agent_roster.cpp:92-99`, `375`).

Painting (`agent_roster.cpp:127-177`):

- Fill: selected → `st::dialogsBgActive`; down or hovered → `st::windowBgRipple`;
  otherwise `st::windowBgOver`.
- Primary line (13pt DemiBold) and secondary line (10pt) pick their pen from
  the same three-state ladder: `st::dialogsNameFg…` / `st::dialogsTextFg…`
  (Active / Over / plain).
- A `PE_FrameFocusRect` is painted when the row has focus.
- The row palette `Highlight` is set to `st::dialogsBgActive` so selection
  color resolves from the same token its paint uses
  (`agent_roster.cpp:384-386`).

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

`rebuild_document` (`conversation_surface.cpp:159-216`) writes one
`QTextBlock` per message in the caller's order:

- Alignment left (incoming) or right (outgoing); margins bound each message to
  `max(0.72 × viewport width, 160px)`, with 12px edge and 4px/18px top/bottom
  margins (`message_block_format`, `conversation_surface.cpp:36-51`).
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
- `resizeEvent` reflows only when the quantized message cap
  (`int(width × 0.72 / 8) × 8`) changes; a live pixel-by-pixel resize does not
  rebuild   (`conversation_surface.cpp:272-284`).
