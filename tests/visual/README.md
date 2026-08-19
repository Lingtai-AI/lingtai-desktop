# Visual snapshot tests

macOS-only PNG baselines live under `baselines/macos/{light,dark}/`.

Failure outputs go to `artifacts/visual/` (gitignored).

Baseline updates are explicit only:

```bash
./scripts/update-ui-baselines.sh
```

Implementation: `VisualTestUtils.*`, `VisualSnapshotHarness.h`.

Baselines live under `baselines/macos/{light,dark}/` as `<snapshot>-<viewport>.png`.
Failure outputs go to `artifacts/visual/<test-name>/` (gitignored).

Baseline updates are explicit only:

```bash
./scripts/update-ui-baselines.sh
```
