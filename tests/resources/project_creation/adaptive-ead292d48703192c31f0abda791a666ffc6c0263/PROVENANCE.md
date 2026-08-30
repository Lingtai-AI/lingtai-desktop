# Adaptive Recipe Test Fixture

These seven files are exact test-only copies of the `adaptive` recipe assets
from the read-only `lingtai-tui` audit worktree at commit
`ead292d48703192c31f0abda791a666ffc6c0263` (Git tree
`1dd7ca97692448c4e28005097e62a4ab19c931ec`).

Desktop does not read this directory at runtime and does not publish `.recipe`
or `.tui-asset` state. Tests use the pinned source material to independently
derive the bounded Desktop-owned adaptation; rendered Desktop guidance is
intentionally not a verbatim copy of these TUI bytes.
