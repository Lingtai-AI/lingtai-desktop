# UI test fixtures

Synthetic project trees for deterministic UI and visual tests.

| Fixture | Purpose |
| --- | --- |
| `empty/` | No agents, minimal `.lingtai` project shell |
| `typical/` | One project, a few agents, normal conversation |
| `dense/` | Many agents and long conversation for layout stress |

Fixtures are populated incrementally. `empty/project/` is a minimal openable
project (one agent); `typical/` and `dense/` remain reserved for later phases.

Tests must never read or write real user projects under `$HOME`.
