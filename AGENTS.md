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
python3 -m unittest tests.test_project_creation_source_contract
python3 -m unittest tests.test_macos_packaging
python3 -m unittest tests.test_app_archive
python3 -m unittest tests.test_desktop_user_cli
python3 -m unittest tests.test_desktop_support_update
python3 -m py_compile scripts/macos_packaging.py scripts/package-macos.py scripts/verify-macos-package.py scripts/app_archive.py scripts/package-app-archive.py scripts/verify-app-archive.py scripts/desktop_user_cli.py scripts/support_bootstrap.py scripts/support_release.py scripts/install-macos-app.py
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
ctest --test-dir build --output-on-failure -R '^preset_catalog$'
ctest --test-dir build --output-on-failure -R '^project_creation$'
ctest --test-dir build --output-on-failure -R '^agent_sleep$'
ctest --test-dir build --output-on-failure -R '^kanban_model$'
ctest --test-dir build --output-on-failure -R '^posix_descriptor_primitives$'
ctest --test-dir build --output-on-failure -R '^workspace_selection$'
ctest --test-dir build --output-on-failure -R '^native_shell(_behavior)?$'
./scripts/smoke.py
```

The primary terminal-install artifact is a portable App archive. Package only
an already verified self-contained universal App, from an isolated checkout:

```bash
python3 scripts/package-app-archive.py \
  --app build/LingTai.app --output-dir /private/tmp/lingtai-desktop-package
python3 scripts/verify-app-archive.py \
  --archive /private/tmp/lingtai-desktop-package/LingTai-0.1.5-macOS-universal.app.tar.gz \
  --manifest /private/tmp/lingtai-desktop-package/LingTai-0.1.5-macOS-universal.app.manifest.json
```

Build and independently revalidate the exact three-file support release set only
into a new disposable output directory:

```bash
python3 scripts/support_release.py build \
  --version 0.1.6 --output /private/tmp/lingtai-desktop-support-0.1.6
python3 scripts/support_release.py validate \
  /private/tmp/lingtai-desktop-support-0.1.6
```

The support set is `support-manifest.json`, `desktop_user_cli.py`, and
`verify-app-archive.py`, all mode 0600. Publication is deterministic and
no-clobber. Its v1 trust when later downloaded is TLS + the exact official
GitHub repository/tag/asset route + manifest hashes, never signing/signatures.

Archive/manifest publication is exclusive and rollback is inode-bound; never
replace its hard-link no-clobber publication with a final rename. Manifest
`packaging_git_*` fields describe only the tracked packaging checkout, not the
App build. The older DMG producer/verifier remains an optional release
experiment; it is not an input to `lingtai-desktop` install or update.

The developer-preview installer is always exercised with an injected fake
HOME. Never run it against a developer's real HOME during validation.
`scripts/desktop_user_cli.py` owns install/update/launch/doctor/uninstall policy;
`scripts/install-macos-app.py` is only a bootstrap wrapper. Default bootstrap and
explicit updates discover only stable releases from the fixed official
`Lingtai-AI/lingtai-desktop` GitHub source; the injected transport keeps every
test offline. Preserve the explicit paired local-artifact path. Normal installed
commands use independent App and support cache cadences. Support cache/provider
errors warn and fail open for ordinary commands; noninteractive support checks
notice only, and interactive checks stage only after a default-No `y`/`yes`.
Explicit official `update` forces support first, canonical reexec, and then App;
a visible support failure may leave support unchanged and continue App. Paired
local App updates must perform zero support transport/cache work. Do not add PATH/profile
mutation, sudo, quarantine/Gatekeeper bypass, another remote source, or a release
claim.
Preserve existing modes and unrelated contents in the shared `.local`,
`.local/bin`, and `.local/share` parents. File publication must be fully
prepared before its no-clobber link becomes visible, rollback may remove only
the exact inode created by that invocation, and uninstall must complete its
whole-tree no-symlink/digest/known-child preflight before its first deletion.
For managed support, retain exact validated payload bytes through self-test/import,
authenticate rollback pointers and journals by inode, and revalidate both planes
before commit. Candidate self-test must remain isolated and mutation-denying;
`uninstall --version` must remain App-only and must not inspect support.

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
