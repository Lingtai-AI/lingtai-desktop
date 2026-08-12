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
src/agent_manifest_discovery.{h,cpp}   immediate read-only manifest discovery
src/agent_manifest_discovery_test_seam.h deterministic filesystem race/error seam
src/compatibility_probe.{h,cpp}        Qt Core-owned read-only compatibility probe
tests/agent_manifest_discovery_test.cpp discovery/no-write behavior contract
tests/project_attachment_test.cpp      real C++ attachment/containment behavior contract
tests/test_project_attachment.py       dependency-free C++ contract compile/run harness
tests/compatibility_probe_test.cpp     compatibility behavior/no-write contract
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
project-attachment seam. The separate `lingtai_desktop_compatibility` library
privately owns Qt Core JSON parsing, and the smoke links it with
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

`discover_agent_manifests` examines only the canonical attachment root's real
`.lingtai` directory and its immediate real child directories. A regular,
non-symlink `.agent.json` is membership evidence: any JSON object is `Valid`,
while a present unsafe, unreadable, nonregular, invalid, or non-object source
stays visible with typed provenance. Missing or disappearing manifests are
omitted. Lossless child names are the stable keys; identity, role, capability,
heartbeat, status, and lifecycle projection are deliberately not parsed. Root
enumeration errors fail closed, results are sorted, and the scanner never
writes or follows root, child-directory, or manifest symlinks. Qt JSON remains
private to the discovery implementation.

`probe_compatibility` reads one explicitly requested relative agent directory
and one explicitly supplied global install-receipt path below an accepted
attachment; it never infers `$HOME` or a project receipt. It recognizes only
the current machine receipt and kernel-resolved-manifest envelopes, reports raw
`init.json` structure and the `bash`/`shell` alias case without rewriting it,
and retains independent typed findings. No requested agent is explicitly
`Degraded`; commands are allowed only for a finding-free requested agent with a
recognized receipt, fresh resolved manifest, and structurally usable raw init.
The probe does not implement the kernel's full raw-init semantics or verify an
installed executable.

Qt is external rather than fetched or committed. Configure resolves the exact
Qt 6.11.1 prefix from `QT_ROOT` or the documented `$HOME/Qt/6.11.1/macos`
default. The offscreen smoke constructs an actual `Ui::RpWidget`, processes the
Qt event loop, emits a success marker, and exits under a timeout.
