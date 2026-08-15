# `src/ui/` — widget contract

The two widget owners in `src/ui/` are **pure presentation**. Normative
boundary: they render caller-provided state and emit callbacks only. Each claim
below is grounded in the widget source.

## Render caller state, emit callbacks — nothing else

- **Render only.** `AgentRoster::set_rows` accepts an already-produced
  `AgentSnapshot` and an optional selected key (`agent_roster.h:37-38`);
  `ConversationSurface::set_conversation` accepts a presentation name and the
  existing direct-conversation rows (`conversation_surface.h:32-34`). Neither
  widget discovers, reads, or derives any project/Agent truth.
- **One explicit custom callback port.** The roster's `RowClickHandler`
  forwards a row's `directory_key` on a click
  (`agent_roster.cpp:370-375`); `ConversationSurface` has no callback port and
  emits nothing. Separately, the Open/New Project child `QPushButton`s expose
  their standard Qt `clicked` signals, which `NativeShell` finds by object name
  and wires externally (`native_shell.cpp:593-603`); the roster composes those
  buttons but connects nothing to them (`agent_roster.cpp:219-240`). This is
  still pure presentation: row selection and the two action buttons are the
  only emissions, and both stay caller-side. Selection, eligibility, and
  routing decisions belong to `NativeShell` in parent `src/`; these widgets
  only render the caller's already-decided `selected_key` and `manifest_kind`.

## Boundary — what these widgets must not do

- **No filesystem reads or writes.** The roster touches paths only as display
  text (`path_text`, `agent_roster.cpp:28-33`); the conversation surface
  touches the filesystem never. No `QFile`, no `QProcess`, no descriptor walk.
- **No project/Agent truth derivation.** The roster never reruns
  `project_agents`; it consumes the snapshot as given and compares only the
  caller's fields to detect an unchanged model (`agent_roster.cpp:320-337`).
  `AgentRowButton`'s enabled/disabled state mirrors the already-projected
  `manifest_kind == valid` (`agent_roster.cpp:362`) — it is display enablement,
  not eligibility evaluation.
- **No business eligibility.** A row click forwards the directory key
  (`agent_roster.cpp:371-375`); whether that key may select a project or start
  an Agent is `NativeShell`'s decision, never this folder's.
- **No subprocesses.** No `QProcess`, no `lingtai` invocation, no launch. (The
  shell's `start_agent`/`ProjectBootstrapRunner` live in parent `src/`.)
- **No duplicate domain model.** The roster stores only the snapshot it was
  given for change detection (`visible_snapshot_`, `agent_roster.h:56`);
  `AgentRowButton` is presentation-only. The conversation surface reuses the
  caller's `DirectConversationMessage` rows verbatim and adds no message or
  conversation model of its own.

## Contracts preserved (only where source proves them)

- **Focus.** Rows are `Qt::StrongFocus` (`agent_roster.cpp:106`); a focused
  enabled row activates on Return/Enter through the same `click()` selection
  path (`agent_roster.cpp:171-180`); `focus_row` targets the enabled row for a
  key or the first enabled row otherwise (`agent_roster.cpp:381-399`). Row
  identity and focus survive an unchanged refresh because the row tree is not
  rebuilt (`agent_roster.cpp:339-342`).
- **Accessibility.** Every composed element sets a static object name and
  accessible name/description — the sidebar (`agent_roster.cpp:203-206`), the
  roster/scroll/rows containers (`agent_roster.cpp:247-269`), each row
  (`agent_roster.cpp:353-358`), and the conversation surface inherits
  `QTextEdit` accessibility (`conversation_surface.cpp:85-98`).
- **Selection.** Rows are checkable and the checked state is caller-driven on
  every refresh (`agent_roster.cpp:360-361`, `298-309`).
- **Copy/selection (conversation).** The surface is `setReadOnly(true)` with
  undo disabled (`conversation_surface.cpp:87-88`) but stays a plain
  `QTextEdit`; `paintEvent` delegates to `QTextEdit::paintEvent` so native
  text selection and copy are preserved (`conversation_surface.cpp:245-248`).
  Message text is inserted literally and never interpreted as markup
  (`conversation_surface.cpp:187-189`).
- **Scroll.** An identical conversation refresh is a no-op
  (`conversation_surface.cpp:134-144`); a changed refresh follows the bottom
  only if the human was already there, else preserves the prior position
  (`conversation_surface.cpp:151-153`, `192-194`). An unchanged roster refresh
  preserves scroll by not rebuilding the row tree.
