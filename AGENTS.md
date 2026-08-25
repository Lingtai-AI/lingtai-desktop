# Working in LingTai Desktop

## Scope

This is a small native-desktop foundation, not a Telegram Desktop fork. Keep
owned product code in `src/`; do not import Telegram account, protocol, chat,
message, media, contact, cache, or high-level UI code.

## Dependency discipline

- Treat `cmake/desktop-app-toolkit-lock.json` as the exact source of dependency
  provenance. Change a pin only as a reviewed, coherent lock update.
- Run `./scripts/bootstrap-deps.sh` rather than manually populating `.deps/`.
  It rejects a dirty, incorrectly remote, or incorrectly pinned existing
  checkout; remove a bad ignored checkout deliberately before retrying.
- The `.deps/`, `build/`, and Qt SDK trees are local build inputs and must never
  be committed.
- The three tdesktop parent icons are fetched by blob ID during bootstrap. Do
  not add binary copies to the repository or replace checksum verification with
  unpinned downloads.

## Validate changes

From the repository root:

```bash
python3 -m unittest tests.test_repository_contract
python3 -m json.tool cmake/desktop-app-toolkit-lock.json >/dev/null
for script in scripts/*.sh; do bash -n "$script"; done
export QT_ROOT="$HOME/Qt/6.11.1/macos"
./scripts/bootstrap-deps.sh
./scripts/configure.sh
./scripts/build.sh
ctest --test-dir build --output-on-failure -R '^project_attachment$'
ctest --test-dir build --output-on-failure -R '^attachment_selection$'
ctest --test-dir build --output-on-failure -R '^agent_projection$'
ctest --test-dir build --output-on-failure -R '^direct_conversation_route$'
ctest --test-dir build --output-on-failure -R '^direct_conversation_history$'
ctest --test-dir build --output-on-failure -R '^direct_mail_publisher$'
ctest --test-dir build --output-on-failure -R '^agent_preset_summary$'
ctest --test-dir build --output-on-failure -R '^agent_setup_store$'
ctest --test-dir build --output-on-failure -R '^tui_executable_resolver$'
ctest --test-dir build --output-on-failure -R '^agent_sleep$'
ctest --test-dir build --output-on-failure -R '^kanban_model$'
ctest --test-dir build --output-on-failure -R '^posix_descriptor_primitives$'
ctest --test-dir build --output-on-failure -R '^workspace_selection$'
ctest --test-dir build --output-on-failure -R '^native_shell(_behavior)?$'
./scripts/smoke.py
```

Keep the one focused repository-contract test focused on pinned dependency
provenance and tracked-artifact hygiene. Add a test only for a clearly
distinct, owned behavior, and prefer a real CMake/ctest target over a
Python wrapper that recompiles a C++ contract ctest already builds and runs.

## UI testing (mac-first)

Full plan: [`tests/UI_TESTING_PLAN.md`](tests/UI_TESTING_PLAN.md).

**Platform decision:** visual baselines and CI gating are **macOS only**.
Linux is out of scope unless it becomes a shipping target.

When UI code changes:

1. Run unit tests and `./scripts/run-ui-tests.sh`.
2. Do not auto-update visual baselines to make tests pass.
3. Inspect diff artifacts first; update baselines only via
   `./scripts/update-ui-baselines.sh` when the visual change is intentional.
4. Prefer widget `objectName` + Qt Test interaction over coordinate automation.
5. Do not weaken global visual diff thresholds.
