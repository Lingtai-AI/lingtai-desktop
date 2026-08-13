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
python3 -m unittest tests.test_project_attachment tests.test_workspace_selection tests.test_repository_contract
if [[ -x /usr/local/bin/g++-14 ]]; then
  CXX=/usr/local/bin/g++-14 python3 -m unittest tests.test_project_attachment tests.test_workspace_selection
else
  printf 'GCC 14 portability gate skipped: /usr/local/bin/g++-14 absent\n'
fi
python3 -m json.tool cmake/desktop-app-toolkit-lock.json >/dev/null
for script in scripts/*.sh; do bash -n "$script"; done
export QT_ROOT="$HOME/Qt/6.11.1/macos"
./scripts/bootstrap-deps.sh
./scripts/configure.sh
./scripts/build.sh
ctest --test-dir build --output-on-failure -R '^compatibility_probe$'
ctest --test-dir build --output-on-failure -R '^agent_manifest_discovery$'
ctest --test-dir build --output-on-failure -R '^agent_roster_presence$'
ctest --test-dir build --output-on-failure -R '^agent_identity_status$'
ctest --test-dir build --output-on-failure -R '^direct_conversation_route$'
ctest --test-dir build --output-on-failure -R '^direct_conversation_history$'
ctest --test-dir build --output-on-failure -R '^direct_mail_publisher$'
ctest --test-dir build --output-on-failure -R '^posix_descriptor_primitives$'
ctest --test-dir build --output-on-failure -R '^workspace_selection$'
ctest --test-dir build --output-on-failure -R '^native_shell(_behavior)?$'
./scripts/smoke.py
```

Keep the one focused repository-contract test focused. Add a test only for a
clearly distinct, owned behavior; dependency configuration is already covered
by the real configure/build/offscreen smoke path.
