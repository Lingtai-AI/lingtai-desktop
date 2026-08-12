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
bash -n scripts/*.sh
export QT_ROOT="$HOME/Qt/6.11.1/macos"
./scripts/bootstrap-deps.sh
./scripts/configure.sh
./scripts/build.sh
./scripts/smoke.py
```

Keep the one focused repository-contract test focused. Add a test only for a
clearly distinct, owned behavior; dependency configuration is already covered
by the real configure/build/offscreen smoke path.
