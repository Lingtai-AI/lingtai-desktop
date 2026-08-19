# UI test fixtures

Synthetic project trees for deterministic UI and visual tests.

| Fixture | Purpose |
| --- | --- |
| `empty/` | No agents, minimal `.lingtai` project shell |
| `typical/` | One project, a few agents, normal conversation |
| `dense/` | Many agents and long conversation for layout stress |

Fixtures are populated in later milestones (Phase 4+). Until then these
directories reserve the layout described in `UI_TESTING_PLAN.md`.

Tests must never read or write real user projects under `$HOME`.
