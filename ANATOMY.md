# Repository anatomy

```
CMakeLists.txt                         top-level source build graph
cmake/desktop-app-toolkit-lock.json    exact toolkit, third-party, Qt, and blob lock
scripts/bootstrap-deps.sh              verified local source/resource bootstrap
scripts/configure.sh                   Qt-aware CMake configure wrapper
scripts/build.sh                       target build wrapper
scripts/smoke.py                       bounded offscreen smoke runner
src/main.cpp                           owned `Ui::RpWidget` executable entry point
src/crl_integration.cpp                minimal parent `crl` update-stream integration
src/project_attachment.{h,cpp}         Qt-independent project-root containment seam
tests/project_attachment_test.cpp      real C++ attachment/containment behavior contract
tests/test_project_attachment.py       dependency-free C++ contract compile/run harness
tests/test_repository_contract.py      focused tracked-tree and lock contract
```

`bootstrap-deps.sh` materializes ignored `.deps/` inputs in three groups:

- `.deps/src/` contains the exact desktop-app toolkit repositories
  (`lib_ui`, `lib_base`, `lib_rpl`, `lib_crl`, `codegen`, and `legal`).
- `.deps/third_party/` contains the exact header/source dependencies used by
  the full `lib_ui` target. `.deps/build/openssl/` contains locally built,
  universal static OpenSSL outputs.
- `.deps/cmake_helpers/` contains checksum-verified helper CMake files, and
  `.deps/tdesktop-resources/` contains the three checksum-verified upstream
  parent resource blobs needed by this pinned `lib_ui` build.

The top-level CMake graph creates the upstream dependency target names expected
by the full pinned `lib_ui` CMake target and adds its complete source tree
without patching it. The Qt-independent `lingtai_desktop_core` library owns the
project-attachment seam; `lingtai_desktop_smoke` links that owned target and
`desktop-app::lib_ui`.
`src/crl_integration.cpp` supplies the bounded, no-emission parent update
producer the smoke needs; it is owned LingTai glue, not a Telegram model.

`ProjectAttachment` accepts an existing directory and retains its canonical
(symlink-resolved) root path. Its `resolve` method accepts nonempty existing
relative paths only; empty and dot-only input is invalid. It rejects absolute
paths and every `..` component, canonicalizes the target, and verifies
component-wise containment so an in-project symlink cannot escape. A path below
a regular file is reported separately from a missing target. Both attachment
and resolution return `ProjectPathFailure` plus any underlying filesystem
error; their `noexcept` API performs no project-tree writes and has no Qt or
Telegram dependency.

`target_not_found` is not a containment verdict and must never be treated as
safe-to-create. Path canonicalization cannot detect that an in-project hard
link shares an inode with a file outside the project. The stored canonical root
is path-stable only, not inode-pinned: replacing the directory at that path can
change what a later resolution observes.

Qt is external rather than fetched or committed. Configure resolves the exact
Qt 6.11.1 prefix from `QT_ROOT` or the documented `$HOME/Qt/6.11.1/macos`
default. The offscreen smoke constructs an actual `Ui::RpWidget`, processes the
Qt event loop, emits a success marker, and exits under a timeout.
