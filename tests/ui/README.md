# Functional UI tests

New Qt widget interaction tests belong here.

Existing journey tests (`tests/native_shell_test.cpp`, etc.) remain at the
top level until migrated; they are labeled `ui` in CTest.

Shared helpers:

- `object_names.h` — canonical `lingtai_*` identifiers for `findChild`
- `UiTestUtils.h` — viewports, widget lookup, theme helpers

Registry: `OBJECT_NAMES.md`
