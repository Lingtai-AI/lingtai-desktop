# `src/ui/` — widget-owner anatomy

```
src/ui/agent_roster.h          AgentRoster    persistent 260px project/Agent list column owner
src/ui/agent_roster.cpp
src/ui/conversation_surface.h  ConversationSurface  read-only, text-selectable conversation surface
src/ui/conversation_surface.cpp
```

This folder holds exactly two widget owners. Both compile into the one
`lingtai_desktop_native_shell` static library (`CMakeLists.txt:176-180`), which
`NativeShell` (`src/native_shell.cpp`, owned by parent `src/`) instantiates and
drives. Neither owner reads a file or derives project/Agent truth; the only
explicit custom callback port is roster row selection. The Open/New Project
child `QPushButton`s expose their standard Qt `clicked` signals, which
`NativeShell` finds by object name and wires externally
(`native_shell.cpp:593-603`). Both render only state the shell hands in, from
readers owned in parent `src/`: `AgentSnapshot` from
`src/agent_projection.{h,cpp}` and `DirectConversationMessage` rows from
`src/direct_conversation_history.{h,cpp}`.

Cross-links: parent owners and the domain readers they consume are mapped in
`../ANATOMY.md`, the normative boundary in `../CONTRACT.md`, and the observed
behavior in `../BEHAVIORS.md`. Repository/build/test navigation lives in the
root `../../ANATOMY.md`.

## `AgentRoster`

The persistent left project/Agent list column. It owns the project identity
header, the compact Open/New Project action row, and the scrollable Agent rows;
the selected-content pane lives outside this owner. Fixed width 260px
(`kRosterColumnWidth`, `agent_roster.cpp:24`). Its visible rows omit the
human pseudo-agent: the shared `AgentSnapshot` keeps the human (routing,
mailbox, and selected-detail truth consume it), and only this presentation
filters the row out.

Public ports (`src/ui/agent_roster.h:30-47`):

- `set_rows(AgentSnapshot, optional<path> selected_key)` — the single data input;
  accepts an already-produced `AgentSnapshot` plus the caller-chosen selected
  directory key. Rebuilds rows only when the visible (human-omitted) model
  changed; an unchanged refresh updates only checked states
  (`agent_roster.cpp:325-396`).
- `set_row_click_handler(RowClickHandler)` — the only explicit custom callback
  port; `RowClickHandler` is `std::function<void(const std::filesystem::path &)>`
  (`agent_roster.h:32`). Each enabled row's `clicked` signal forwards its
  `directory_key` through the handler (`agent_roster.cpp:388-392`).
- `focus_row(optional<path> key = nullopt)` — keyboard focus: focuses the
  enabled row for `key`, else the first enabled row; the narrow OneColumn Back
  path hands navigation back to the roster through this (`agent_roster.cpp:398-414`).
- Non-copyable (`agent_roster.h:37-38`).

View-only state (`agent_roster.h:55-60`): the stored handler, the roster status
`QLabel`, the `QScrollArea`, the rows `QVBoxLayout`, and the last accepted
`AgentSnapshot` (`visible_snapshot_`) used only to detect an unchanged model.
There is no selected-key member: selection is caller-provided on every
`set_rows`/refresh and re-derived from it.

Layout composition (`agent_roster.cpp:214-290`): brand label, project-root
label, the Open/New action row (the shell wires their clicks; the owner only
composes them, `agent_roster.cpp:236-252`), a Workspace label, an Agents
heading, the roster status label, and a scroll area holding the rows widget.
Every element carries a static accessible/object name used by the shell's
semantic tests.

Painting (`agent_roster.cpp:293-296` and `130-230`): the owner fills the
`st::windowBgOver` list field; each `AgentRowButton` (a checkable `QPushButton`)
derives its intrinsic preferred height from the larger of a fixed 40px avatar
(`kAvatarDiameter`) and the two 13pt line metrics (`QFontMetrics` heights for
13pt DemiBold + 13pt Normal), plus the 8px vertical frame top/bottom. It paints
selected/hover/pressed/plain fill, a fixed leading circular avatar/initial
region, and paint-time right-elided two-line text from the shared lib_ui
palette tokens (`st::dialogsBgActive`, `st::windowBgRipple`,
`st::dialogsNameFg…`, `st::dialogsTextFg…`) plus a focus rect when focused. The
button's full text, accessible name/description, and properties stay untouched.

Build ownership: `CMakeLists.txt:179` compiles `src/ui/agent_roster.cpp` into
`lingtai_desktop_native_shell`.

## `ConversationSurface`

A read-only, text-selectable conversation surface for the selected Agent. It
stays a plain `QTextEdit` so inherited selection, copy, and accessibility
behavior are preserved (`src/ui/conversation_surface.h:23`).

Public ports (`src/ui/conversation_surface.h:26-37`):

- `set_conversation(QString them, vector<DirectConversationMessage> messages)` —
  the data input; `them` is the caller-chosen presentation name for incoming
  rows. Replaces the document from the existing direct rows in their accepted
  order; an identical refresh is a no-op that preserves scroll, selection, and
  focus (`conversation_surface.cpp:147-157`).
- `set_plain_state(QString text)` — one plain centered state for the
  selection/no-route/empty cases (`conversation_surface.cpp:113-127`).
- `attachment_action_requested(request, reveal)` — presentation-only signal
  carrying message id, ordered index, and displayed identity to the native
  shell; the widget launches nothing.

View-only state: the last accepted `them_`, `last_messages_`,
`last_plain_state_`, the quantized `last_layout_width_` used to avoid reflowing
on every pixel of a live resize, and one phase-driven wheel-gesture flag plus
the existing deferred-pin generation. The flag is necessary because a
pixel-precise trackpad gesture can begin without moving `QScrollBar`'s integer
value. There is no document model here beyond what Qt's `QTextDocument` owns;
blocks are rebuilt programmatically from the caller rows.

Painting/layout (`conversation_surface.cpp:239-307`): `paintEvent` fills the
viewport with the `st::windowBgOver` chat backdrop, paints rounded bubbles
(`st::msgOutBg`/`st::msgInBg`, radius 8) behind message blocks, then delegates
to `QTextEdit::paintEvent` so text keeps native scroll translation, selection,
and copy. `resizeEvent` quantizes the viewport/layout width and reflows only
when that bound meaningfully changes — following the full layout width, not
just the capped message width, because a wide pane's centered column gutters
keep moving after the message cap stops. `rebuild_document`
(`conversation_surface.cpp:180-237`) writes one `QTextBlock` per message
(incoming left / outgoing right) with a header line (author 15px DemiBold plus
the timestamp at 13px Normal), an optional subject line (13px Medium), and a
literal body (14px Normal). Messages live inside a centered shared reading
column (maximum 900px): at the explicit narrow breakpoint (below 480px) each
block is near-full (viewport minus the two 12px edge gutters); otherwise the
ordinary 72% ratio applies with the 160px lower bound and the absolute
readable cap of 640px, and one outer gutter plus one inner remainder are
derived and cross-assigned so incoming stays left-anchored and outgoing
right-anchored inside the same column (`message_block_format` and
`message_block_width`, `conversation_surface.cpp:44-72`).

Attachment presentation uses ordinary message-frame blocks after the body.
The document owns wrapping and scroll geometry; the painter adds the rounded
card surface, and inline `QTextImageFormat` resources are bounded by the shared
thumbnail helper. Open/Reveal are semantic anchors, not child widgets, so they
have no separate `objectName` and retain the conversation surface's inherited
text accessibility.

Viewport wheel observation (`eventFilter`) cancels any queued automatic
bottom pin before native handling and tracks `ScrollBegin`/updates/momentum
through `ScrollEnd`. Append and rebuild follow the bottom only outside that
active interval. The filter never consumes wheel events; `QTextEdit` remains
the scrolling, kinetic-motion, selection, copy, and accessibility owner.

Build ownership: `CMakeLists.txt:180` compiles `src/ui/conversation_surface.cpp`
into `lingtai_desktop_native_shell`.
