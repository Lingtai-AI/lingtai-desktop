# Retained visual baseline data

The visual comparison executable and helpers have been retired. No `visual`
ctests are registered. Baseline PNGs under `baselines/macos/{light,dark}/` are
left untouched; their presence is not a runnable visual gate.

Runner and baseline-update scripts remain unchanged, but their empty visual
selection produces no appearance evidence. Human-visible macOS acceptance is
required; do not update these assets to manufacture a passing result. See
[`../ANATOMY.md`](../ANATOMY.md) for the active smoke inventory.
