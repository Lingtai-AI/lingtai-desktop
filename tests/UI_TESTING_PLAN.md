# Mac-first UI testing plan

This document records the agreed plan for automated UI testing in
`lingtai-desktop`. It is the source of truth for future agents and contributors.

**Last updated:** 2026-08-19

## Product decision

- **Target platform:** macOS only for visual baselines and CI gating.
- **Linux is out of scope** unless it becomes a shipping platform or support
  comes for free as a byproduct.
- Users and developers work on macOS; screenshots and CI must match that
  environment.

## Goal

After any UI change, one command should answer:

1. Does the UI still **work**?
2. Does the UI still **look right**?

```text
Agent changes UI
        ↓
./scripts/run-ui-tests.sh
        ↓
┌───────────────────┴───────────────────┐
│                                       │
Functional Qt tests              Visual Qt tests
(click / type / state)           (QWidget::grab + PNG diff)
        │                                       │
        └───────────────────┬───────────────────┘
                            ↓
                          PASS / FAIL
                            ↓
                       open PR
                            ↓
                   GitHub Actions (macOS)
                            ↓
                 ┌──────────┴──────────┐
                 │                     │
               PASS                  FAIL
                 │                     │
            mergeable          screenshots
                               diff.png
                               metadata.json
                               logs
                               video (optional, later)
```

## What we are building

| Layer | Purpose |
| --- | --- |
| **Functional UI tests** | Click/type/navigate; assert real state changes |
| **Visual snapshot tests** | Grab widget pixels; compare to committed baselines |
| **Deterministic test mode** | Same fixture + theme + size → same behavior/pixels |
| **Direct page construction** | Jump to a screen without full onboarding journey |
| **macOS CI** | PR gate on the same OS users see |
| **Explicit baseline update** | Never auto-accept visual changes |

## What we are not building (MVP)

- Squish or other commercial GUI automation
- Coordinate-based desktop automation
- Linux baselines or Linux CI as the merge gate
- One giant end-to-end test for everything
- Automatic baseline regeneration during normal test runs
- One baseline shared across macOS / Linux / Windows
- Weakening global diff thresholds to make CI green

---

## Milestone 1 — Foundation

### Phase 0 — Inspect before changing

Document (once, then keep updated):

- Qt version, Widgets vs QML, build system
- Existing test harness and CI
- Theme selection, `NativeShell` construction
- Persistent state, timers, background refresh, network
- Which tests are unit vs UI vs visual today

### Phase 1 — Directory structure

```text
tests/
├── unit/
├── ui/
├── visual/
│   ├── VisualTestUtils.*
│   └── baselines/
│       └── macos/
│           ├── light/
│           └── dark/
└── fixtures/

scripts/
├── run-ui-tests.sh
├── update-ui-baselines.sh
└── ci/
    ├── run-ui-macos.sh
    └── record-ui-tests.sh   # optional, later

artifacts/                   # gitignored
├── visual/
├── logs/
└── videos/
```

- Baselines **committed**; artifacts **ignored**.

### Phase 2 — Stable widget identifiers

Use `objectName` on interactive controls, major panels, dynamic lists, and
stateful elements — not on every static label.

Existing convention: `lingtai_*` names (e.g. `lingtai_composer`,
`lingtai_agent_detail`).

---

## Milestone 2 — Deterministic test mode

### Phase 3 — Centralized UI test mode

Extend `RuntimeOptions` (or equivalent) — do **not** scatter `if (testMode)`
across the codebase.

```cpp
struct RuntimeOptions {
    bool ui_test_mode = false;
    bool animations_enabled = true;
    bool network_enabled = true;
    // ...
};
```

Entry: `--ui-test` and/or `APP_UI_TEST_MODE=1`.

In UI test mode: animations off, external/network/telemetry off or mocked,
fixture startup, deterministic timestamps where displayed.

**Visual tests must render real production widgets**, not a fake minimal tree.
(The existing `deterministic_ui` minimal constructor is only for narrow widget
contract tests, not for visual baselines.)

### Phase 4 — Direct constructibility

Tests should open the screen under test directly:

```text
load fixture → NativeShell / page with test deps → show target page → test
```

Navigation journeys are tested separately from page rendering/screenshots.

---

## Milestone 3 — Functional UI testing

### Phase 5 — Functional tests

Use Qt Test mouse/keyboard simulation where possible. Assert **application
state**, not only that a button disappeared.

Examples: repository/model changed, selection changed, page visibility,
composer enablement.

### Phase 6 — Canonical viewport sizes

Fixed sizes (adjust once if app minimum size requires it):

| Name | Size |
| --- | --- |
| compact | 1000 × 700 |
| normal | 1280 × 800 |
| wide | 1600 × 1000 |

**PR default:** normal / light + normal / dark.

**Later (nightly):** compact and wide for layout-sensitive pages.

---

## Milestone 4 — Visual snapshot infrastructure

### Phase 7 — Capture

`QWidget::grab()` on the widget under test (not full desktop screenshots).

Wait for real ready conditions (`QTRY_VERIFY`, model loaded, list count);
avoid large arbitrary `qWait` sleeps.

### Phase 8 — Compare

Central config:

```text
CHANNEL_THRESHOLD = 10
MAX_CHANGED_RATIO = 0.001   # 0.1%
```

Return: pass/fail, changed pixels, ratio, paths.

### Phase 9 — Failure artifacts

Per failed test:

```text
artifacts/visual/<test-name>/
├── baseline.png
├── actual.png
├── diff.png
└── metadata.json
```

### Phase 10 — Baseline updates (manual only)

```bash
./scripts/update-ui-baselines.sh
# or
UPDATE_UI_BASELINES=1 ctest -L visual
```

Normal `ctest -L visual` must **never** write baselines.

---

## Milestone 5 — First valuable suite

### Phase 11 — ~10–20 high-value snapshots

Start small, e.g.:

- main-window-empty, main-window-conversation
- preset-list-empty, preset-list-populated
- preset-editor-new, preset-editor-populated
- settings-default

Control explicitly: fixture, theme, viewport, selection, focus, scroll.

### Phase 12 — CTest labels

```bash
ctest -L unit --output-on-failure
ctest -L ui --output-on-failure
ctest -L visual --output-on-failure
```

Labels: `unit`, `ui`, `visual` (visual tests also carry `ui`).

---

## Milestone 6 — Local workflow

### Phase 13 — One command

```bash
./scripts/run-ui-tests.sh
```

Build if needed, run functional + visual, print summary, point to artifacts
on failure. **This is the primary interface for coding agents after UI edits.**

### Phase 14 — macOS failure recording (optional, later)

Record during test run; keep video only on failure. Not required for MVP.

Priority: assertions → images → logs → video.

---

## Milestone 7 — macOS CI

### Phase 15 — GitHub Actions

- Workflow: `.github/workflows/ui-tests.yml`
- Runner: pinned macOS (e.g. `macos-15` or project-standard version)
- Steps: build → `./scripts/ci/run-ui-macos.sh` → upload artifacts on failure
- Triggers: pull_request, push to main

### Phase 16 — PR vs nightly

| Scope | PR | Nightly |
| --- | --- | --- |
| Unit | yes | yes |
| Functional UI | yes | yes |
| Visual | important states, normal light/dark | full matrix: compact/normal/wide × light/dark |

---

## Milestone 8 — Agent rules

Whenever modifying UI code:

1. Run existing unit tests.
2. Run `./scripts/run-ui-tests.sh`.
3. Do **not** update visual baselines merely to make tests pass.
4. Inspect visual diffs first.
5. If the visual change is intentional, regenerate affected baselines
   explicitly and inspect the PNG before committing.
6. Test compact, normal, and wide when changing layout code.
7. Do not introduce fixed-coordinate GUI tests.
8. Do not weaken global visual thresholds to accommodate a regression.

---

## Milestone 9 — Acceptance criteria

All must pass:

```bash
ctest -L unit
ctest -L ui
ctest -L visual
./scripts/run-ui-tests.sh
```

Then verify:

1. **Visual:** deliberate padding/spacing change → fail with actual/baseline/diff.
2. **Functional:** break Save (or equivalent) → functional test fails.
3. **Restore** → pass again.
4. **PR** on macOS CI reproduces the same behavior.

---

## Recommended delivery order

| Step | Milestones / phases | Outcome |
| --- | --- | --- |
| **A** | 1 (structure, labels), 13 (run script) | Runnable skeleton |
| **B** | 2 (test mode), 3–4 (determinism + direct open), 5 (first functional) | Behavior checks |
| **C** | 7–11 (visual utils + first baselines) | Screenshot regression |
| **D** | 15–16 (macOS CI, PR/nightly split) | Automated PR gate |
| **E** | 14, 17–18 (video, agent rules, validation) | Polish + proof |

---

## Current implementation status (2026-08-19)

Partial work already landed; treat as inputs to this plan, not completion:

| Item | Status |
| --- | --- |
| `RuntimeOptions` + `resolve_runtime_options` | **Done** (`ui_test_mode`, `--ui-test`, env vars, activity timer guard) |
| Phase 4 harness (`UiTestHarness.h`, empty fixture, `ui_test_harness`) | **Done** |
| Phase 5–6 functional test + viewports (`ui_agent_detail_pages`) | **Done** |
| Phases 7–10 visual utils + baseline update flow | **Done** |
| Phases 11–12 first visual snapshots + `visual` labels | **Done** (2 snapshots, normal/light) |
| Phase 13 `run-ui-tests.sh` | **Done** |
| Phases 15–16 macOS GitHub Actions | **Done** (`.github/workflows/ui-tests.yml`; not yet green on remote) |
| `AgentDetailView` extraction + delegation | Done |
| Headless widget test `agent_detail_view_test` | Done (narrow contract, not full visual suite) |
| Stable `lingtai_*` objectNames | Largely present; registry in `tests/ui/OBJECT_NAMES.md` + `object_names.h` |
| CTest labels `unit` / `ui` / `visual` | **Done** (`unit` ×13, `ui` ×10, `visual` ×1) |
| Phase 1 dirs + scripts | **Done** |
| Full `native_shell_behavior` suite in headless agent sandboxes | **Blocker** (QRhi / no display; use macOS + cocoa) |
| CI workflow first green run on GitHub | **Blocker to verify** (Qt path, bootstrap, runner display) |
| Phase 11 expansion (compact/wide/dark, ~10–20 snapshots) | Not yet |
| Phase 14 failure video | Deferred (optional) |

Next engineering priority for mac CI: make full `NativeShell` / `Ui::RpWindow`
startup reliable under macOS test runners without changing production behavior
(test-mode guards only).

---

## Related docs

- [`tests/CONTRACT.md`](CONTRACT.md) — evidence rules for all tests
- [`tests/ANATOMY.md`](ANATOMY.md) — what tests exist today
- [`../AGENTS.md`](../AGENTS.md) — agent workflow including UI test command
