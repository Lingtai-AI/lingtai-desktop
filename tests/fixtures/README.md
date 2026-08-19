# UI test fixtures

Synthetic project trees for deterministic UI and visual tests.

| Fixture | Purpose |
| --- | --- |
| `empty/` | No agents, minimal `.lingtai` project shell |
| `typical/` | One human + one agent (`alpha`), enables composer and page tests |
| `dense/` | Many agents and long conversation for layout stress |

Fixtures are populated incrementally. `empty/project/` is a minimal openable
project (one agent); `typical/project/` adds human + alpha for composer and
page-switch tests; `dense/` remains reserved for layout stress.

Tests must never read or write real user projects under `$HOME`.
