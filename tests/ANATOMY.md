---
related_files:
  - ANATOMY.md
  - src/ANATOMY.md
  - tests/CONTRACT.md
  - CMakeLists.txt
  - tests/test_repository_contract.py
  - tests/test_native_shell.py
  - tests/direct_conversation_history_test.cpp
  - tests/direct_mail_publisher_test.cpp
  - tests/direct_mail_kernel_bridge.py
  - tests/dock_badge_test.cpp
  - tests/composer_draft_test.cpp
  - tests/agent_lifecycle_real_smoke.cpp
maintenance: |
  Keep related_files as repo-relative paths to real files. Include neighboring
  ANATOMY.md files so the anatomy graph stays connected rather than isolated;
  anatomy links must be bidirectional. If you create a new ANATOMY.md, copy this
  maintenance field. If you notice drift between this anatomy and the code,
  report it. See lingtai-dev-guide for details.
---

# `tests/` anatomy

The remaining surface is a small real-process/real-filesystem smoke set, not a
replacement for the retired suites. Source ownership is in
[`../src/ANATOMY.md`](../src/ANATOMY.md); top-level build ownership is in
[`../ANATOMY.md`](../ANATOMY.md). Evidence rules remain in
[`CONTRACT.md`](CONTRACT.md). Behavior documents keep historical anchors and
must not be mistaken for current runnable registration.

## Executable inventory

| File | Build target / ctest | Concrete retained value |
| --- | --- | --- |
| `test_native_shell.py` | App target `lingtai_desktop_smoke`; ctest `native_shell`, label `ui` | Launch actual built App: ordered ready/full-target markers, zero exit, constructed/shown startup, and normal-mode persistence. No widget-name or cosmetic-log assertions. |
| `direct_conversation_history_test.cpp` | `lingtai_direct_conversation_history_test`; ctest `direct_conversation_history`, label `unit` | Real mailbox files: incoming/outgoing text and order, exact selected identity, scalar recipient, unrelated mail exclusion, outbox/sent duplicate collapse, no writes and intermediate-symlink read refusal. |
| `direct_mail_publisher_test.cpp` | `lingtai_direct_mail_publisher_test`; ctest `direct_mail_publisher`, label `unit` | Actual outbox leaf/envelope publication, successive-send no-clobber, unrelated-byte preservation, blocked/symlinked outbox refusal, attachment-only/mixed mail with real private copies and future sent paths. |
| `dock_badge_test.cpp` | `lingtai_dock_badge_test`; ctest `dock_badge`, label `ui` | Offscreen `ShellHost` with an injected sink (never the OS Dock): initial 0, no repeat at the zero baseline, two real inbound rows publish 2, teardown forces 0, and >99/`size_t` max conversion stays numeric/saturated. |
| `composer_draft_test.cpp` | `lingtai_composer_draft_test`; ctest `composer_draft`, label `ui` | Offscreen `ShellHost` through the real project-open selection route: A text+attachment → B → A restores A, B restores B, same-Agent reopen/refresh keeps typing, same Agent name in two projects stays separate, a cleared draft does not resurrect, a failed send preserves and a queued send discards only the sent draft. |
| `test_repository_contract.py` | manual/CI `python3 -m unittest tests.test_repository_contract` | Canonical product version, pinned dependency provenance, tracked-artifact hygiene; retained because unchanged CI explicitly invokes it. |
| `agent_lifecycle_real_smoke.cpp` | `lingtai_agent_lifecycle_real_smoke`; manual only | Production controller, process adapter and detached launcher against disposable real-kernel Agents: creation/attach/setup, start, sleep, suspend, CPR, clear, refresh, aggregate failure and cleanup. |

The wire ctests receive `build/direct-conversation-history-fixture` and
`build/direct-mail-publisher-fixture` as their sandbox argv; each resets and
removes its injected root. Never invoke them with a developer-owned directory.
The subprocess test uses Cocoa on macOS and offscreen elsewhere. On macOS it
still needs a functioning Cocoa session; `--offscreen` is the application's
mode, not proof of packaged-App/visible interaction acceptance.

## Supported build and unattended smoke route

Use the verified dependencies and official Qt 6.11.1 prefix described by
`../AGENTS.md` and `../README.md`. This pruning does not change bootstrap or
configuration policy. With those local prerequisites already available:

```bash
export QT_ROOT="$HOME/Qt/6.11.1/macos"
./scripts/configure.sh
cmake --build build --parallel 8 --target \
  lingtai_desktop_smoke lingtai_direct_conversation_history_test \
  lingtai_direct_mail_publisher_test lingtai_dock_badge_test \
  lingtai_composer_draft_test \
  lingtai_agent_lifecycle_real_smoke
python3 -m unittest tests.test_repository_contract
ctest --test-dir build --output-on-failure \
  -R '^(native_shell|direct_conversation_history|direct_mail_publisher|dock_badge|composer_draft)$'
```

`./scripts/smoke.py` remains an unchanged convenience wrapper around the same
App `--smoke`, not an additional proof layer. Existing runner scripts and CI
triggers/failure handling remain unchanged. `unit` and `ui` still select actual
remaining tests; no visual tests are registered. The runner's pre-existing
empty-visual branch is not visual acceptance.

## Explicit real-kernel lifecycle acceptance

This is **not** part of ctest. Only an authorized native acceptance owner may
run it, using an existing absolute configured-runtime interpreter and a newly
allocated disposable HOME/project. Do not install a runtime or use a live
project merely to make this harness pass.

```bash
# RUNTIME_PYTHON must already name the authorized absolute runtime interpreter.
ACCEPTANCE_ROOT="$(mktemp -d /private/tmp/lingtai-desktop-lifecycle.XXXXXX)"
mkdir "$ACCEPTANCE_ROOT/home"
env -u PYTHONPATH HOME="$ACCEPTANCE_ROOT/home" PATH=/usr/bin:/bin \
  ./build/lingtai_agent_lifecycle_real_smoke \
  "$ACCEPTANCE_ROOT/project" "$RUNTIME_PYTHON"
```

The harness refuses a PATH containing executable `lingtai-tui`, writes only its
caller-owned isolated project/HOME by design, and checks exact-process cleanup
on its successful path. Its exceptional path is not a guaranteed cleanup
transaction: after failure, the operator must inspect exact observed Agent PIDs
before any further action. Keep acceptance receipts; no broad kill or automatic
cleanup is authorized by this command. Its fixture manifest disables tool/model
work and uses a placeholder API key; this is real process/lifecycle evidence,
not a live-provider call or provider acceptance.

## Deliberate coverage loss and retained data

The synthetic shell, hidden-widget, typography/layout, visual comparison,
native popup event, broad domain/state-machine, static-source, and injected
installer/support/package suites are retired. Reader worker generation/counter
and attachment matrices, and publisher empty/source/limit/rollback matrices
are also retired. None is re-proven by startup smoke.

Fonts, visual baselines, static fixtures and pinned project-creation resources
remain untouched, as do CI, installer, setup, configuration and production code.
They are retained data, not active test apparatus. Human-visible macOS
interaction/appearance, native clipboard/picker/tray/quit behavior, packaged-App
acceptance, live-provider behavior, and installer/support transaction acceptance
remain separate unfulfilled gates until an acceptance owner supplies receipts.

## Explicit offline Desktop → kernel reply bridge

`direct_mail_publisher` additionally parses real published JSON to verify the
exact absolute return route, missing/real human ids, attachments, and two
projects sharing human/Agent short names. Its `--bridge-fixtures` mode leaves
only its disposable producer fixtures for the manual bridge below.

Run with an existing absolute Python interpreter and an explicit clean kernel
source checkout pinned to `9506b210ac57264e849bc1c505b44d480f2761fe`:

```bash
"$KERNEL_PYTHON" tests/direct_mail_kernel_bridge.py \
  --kernel-source "$KERNEL_SOURCE" \
  --publisher "$BUILD_DIR/lingtai_direct_mail_publisher_test" \
  --sandbox "$NEW_DISPOSABLE_SANDBOX"
```

The harness verifies the source commit/cleanliness and imported module paths;
its JSON receipt reports the exact commit and interpreter (package metadata
version is informational and may describe an installed distribution). It calls
real C++ `send_direct_mail`, then real `PosixFilesystemMailAdapter` one-shot
pseudo-outbox pickup/serialization and `EmailManager.handle(action="reply")`.
The producer payload is not patched or reconstructed. The only replacement is
a recording final mail transport; real mailman threads are joined with a
bounded timeout. It verifies unchanged route/payload, moved attachment bytes,
exact per-project human destination, and old route-less rejection before
`_send`. No Agent/provider/App starts and no actual delivery occurs. This is
manual offline integration evidence, separate from ctest and installed-App or
UI acceptance. No dependency installation is part of this command.
