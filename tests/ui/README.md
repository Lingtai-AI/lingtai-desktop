# Functional UI tests

New Qt widget interaction tests belong here.

Existing journey tests (`tests/native_shell_test.cpp`, etc.) remain at the
top level until migrated; they are labeled `ui` in CTest.

Shared helpers:

- `object_names.h` — canonical `lingtai_*` identifiers for `findChild`
- `UiTestUtils.h` — viewports, widget lookup, theme helpers
- `UiTestHarness.h` — load fixture → show shell → open project (Phase 4)
- `standardViewports()` — compact / normal / wide sizes (Phase 6)

Registry: `OBJECT_NAMES.md`

Functional tests in this directory:

- `agent_detail_pages_test.cpp` — Presets page switch + `/presets` slash at all viewports
