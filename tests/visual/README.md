# Visual snapshot tests

macOS-only PNG baselines live under `baselines/macos/{light,dark}/`.

Failure outputs go to `artifacts/visual/` (gitignored).

Baseline updates are explicit only:

```bash
./scripts/update-ui-baselines.sh
```

Implementation lands in Milestone 4 (`VisualTestUtils`).
