#!/usr/bin/env python3
"""Fail-closed user-level installer and launcher for LingTai Desktop on macOS."""

from __future__ import annotations

import argparse
import ctypes
import dataclasses
import hashlib
import json
import os
import plistlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import uuid
from contextlib import contextmanager
from pathlib import Path
from typing import Callable, Iterator, Mapping, Sequence


APP_NAME = "LingTai.app"
BUNDLE_ID = "ai.lingtai.desktop"
MINIMUM_MACOS = "13.0"
ARCHITECTURES = ("arm64", "x86_64")
VERSION_PATTERN = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")
SHA_PATTERN = re.compile(r"[0-9a-f]{64}")
GIT_SHA_PATTERN = re.compile(r"[0-9a-f]{40}")
RECEIPT_SCHEMA = 1
LAUNCHER_MARKER = "# lingtai-desktop-owned-v1"
DIAGNOSTIC_SIGNING = "ad-hoc App; unsigned DMG (diagnostic only)"
DIAGNOSTIC_NOTARIZATION = "not performed (diagnostic mode)"
RELEASE_SIGNING = "Developer ID Application; hardened runtime; timestamped"
RELEASE_NOTARIZATION = "Apple accepted; ticket stapled and validated"
MANIFEST_KEYS = {
    "architectures", "file_name", "minimum_macos", "notarization",
    "packaging_git_dirty", "packaging_git_sha", "packaging_git_tree",
    "sha256", "signing", "size_bytes", "version",
}
RECEIPT_KEYS = {
    "schema_version", "version", "bundle_id", "minimum_macos",
    "architectures", "source_dmg", "manifest_sha256", "signing",
    "notarization", "packaging_git_sha", "packaging_git_tree",
    "packaging_git_dirty", "bundle_tree_sha256", "classification",
    "managed_app_path",
}
_FAILPOINT: str | None = None  # Tests inject failures without production flags.


class DesktopCLIError(RuntimeError):
    """A bounded installer/launcher failure safe to print to a terminal."""


@dataclasses.dataclass(frozen=True)
class Manifest:
    version: str
    file_name: str
    size_bytes: int
    sha256: str
    signing: str
    notarization: str
    packaging_git_sha: str
    packaging_git_tree: str
    packaging_git_dirty: bool
    manifest_sha256: str
    diagnostic: bool


@dataclasses.dataclass(frozen=True)
class ManagedPaths:
    home: Path
    local: Path
    bin: Path
    root: Path
    cli: Path
    versions: Path
    receipts: Path
    current: Path
    launcher: Path


@dataclasses.dataclass(frozen=True)
class UninstallEntry:
    version: str
    receipt: dict[str, object]
    version_identity: tuple[int, int]
    receipt_identity: tuple[int, int]


@dataclasses.dataclass(frozen=True)
class UninstallPlan:
    entries: tuple[UninstallEntry, ...]
    root_identity: tuple[int, int]
    cli_identity: tuple[int, int]
    versions_identity: tuple[int, int]
    receipts_identity: tuple[int, int]
    launcher_identity: tuple[int, int]
    module_identity: tuple[int, int]
    verifier_identity: tuple[int, int]
    current_target: str | None
    current_identity: tuple[int, int] | None


def _run(arguments: Sequence[os.PathLike[str] | str], *, label: str,
         environment: Mapping[str, str] | None = None, timeout: int | None = None) -> str:
    try:
        result = subprocess.run(
            [os.fspath(value) for value in arguments], env=dict(environment) if environment else None,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            timeout=timeout, check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise DesktopCLIError(f"{label} timed out") from error
    except OSError as error:
        raise DesktopCLIError(f"{label} could not start") from error
    if result.returncode:
        output = (result.stdout or "").replace("\0", "").strip()[-1200:]
        raise DesktopCLIError(f"{label} failed (exit {result.returncode})" + (f": {output}" if output else ""))
    return result.stdout or ""


class Platform:
    """Injectable macOS process boundary; policy never uses a shell string."""

    def verify_package(self, repository_root: Path, dmg: Path, version: str, release_ready: bool) -> None:
        verifier = repository_root / "scripts/verify-macos-package.py"
        if not verifier.is_file():
            verifier = repository_root / "verify-macos-package.py"
        _regular_nofollow(verifier, "independent package verifier")
        command: list[os.PathLike[str] | str] = [
            sys.executable, verifier,
            "--dmg", dmg, "--expected-version", version,
        ]
        if release_ready:
            command.append("--require-release-ready")
        _run(command, label="independent package verification")

    @contextmanager
    def mounted_app(self, dmg: Path, scratch: Path) -> Iterator[Path]:
        mountpoint = scratch / "mounted"
        mountpoint.mkdir(mode=0o700)
        attached = False
        primary: BaseException | None = None
        try:
            _run([
                "/usr/bin/hdiutil", "attach", "-readonly", "-nobrowse", "-noautoopen",
                "-mountpoint", mountpoint, "-plist", dmg,
            ], label="read-only DMG mount")
            attached = True
            mounted = _run(["/sbin/mount"], label="mounted-volume inspection")
            lines = [line for line in mounted.splitlines() if os.fspath(mountpoint) in line]
            if not lines or not any("read-only" in line for line in lines):
                raise DesktopCLIError("DMG mount is not read-only")
            apps = list(mountpoint.glob("*.app"))
            app = mountpoint / APP_NAME
            if apps != [app] or app.is_symlink() or not app.is_dir():
                raise DesktopCLIError("DMG must contain one exact LingTai.app")
            yield app
        except BaseException as error:
            primary = error
            raise
        finally:
            if attached:
                try:
                    _run(["/usr/bin/hdiutil", "detach", mountpoint], label="DMG detach")
                except DesktopCLIError as detach_error:
                    if primary is None:
                        raise
                    if hasattr(primary, "add_note"):
                        primary.add_note(str(detach_error))

    def copy_app(self, source: Path, destination: Path) -> None:
        _run(["/usr/bin/ditto", "--rsrc", "--extattr", "--acl", source, destination],
             label="staged App copy")

    def smoke(self, executable: Path, fake_home: Path, fake_tmp: Path) -> None:
        environment = {
            "HOME": os.fspath(fake_home), "TMPDIR": os.fspath(fake_tmp),
            "PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "LANG": "en_US.UTF-8",
            "LC_ALL": "en_US.UTF-8",
        }
        output = _run([executable, "--smoke"], label="staged App smoke",
                      environment=environment, timeout=15)
        markers = ("LINGTAI_NATIVE_SHELL_READY", "LINGTAI_LIB_UI_FULL_TARGET_SMOKE_OK")
        positions = [output.find(marker) for marker in markers]
        if any(value < 0 for value in positions) or positions != sorted(positions):
            raise DesktopCLIError("staged App smoke markers are absent or out of order")

    def open_app(self, app: Path) -> None:
        _run(["/usr/bin/open", app], label="LingTai Desktop launch")

    def exec_app(self, executable: Path, arguments: list[str]) -> None:
        os.execv(executable, [os.fspath(executable), *arguments])


def _regular_nofollow(path: Path, label: str) -> os.stat_result:
    try:
        facts = path.lstat()
    except OSError as error:
        raise DesktopCLIError(f"{label} must be an existing regular file, not a symlink") from error
    if stat.S_ISLNK(facts.st_mode) or not stat.S_ISREG(facts.st_mode):
        raise DesktopCLIError(f"{label} must be an existing regular file, not a symlink")
    return facts


def _read_bytes_nofollow(path: Path, label: str, limit: int | None = None) -> bytes:
    _regular_nofollow(path, label)
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
        with os.fdopen(descriptor, "rb") as stream:
            data = stream.read(-1 if limit is None else limit + 1)
    except OSError as error:
        raise DesktopCLIError(f"could not read {label}") from error
    if limit is not None and len(data) > limit:
        raise DesktopCLIError(f"{label} is too large")
    return data


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sha256_file(path: Path, label: str) -> str:
    _regular_nofollow(path, label)
    digest = hashlib.sha256()
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
        with os.fdopen(descriptor, "rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise DesktopCLIError(f"could not hash {label}") from error
    return digest.hexdigest()


def load_manifest(dmg: Path, manifest: Path, *, allow_diagnostic: bool) -> Manifest:
    dmg = Path(dmg)
    manifest = Path(manifest)
    dmg_stat = _regular_nofollow(dmg, "DMG")
    raw = _read_bytes_nofollow(manifest, "manifest", 4096)
    try:
        data = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise DesktopCLIError("manifest must be bounded valid JSON") from error
    if not isinstance(data, dict) or set(data) != MANIFEST_KEYS:
        raise DesktopCLIError("manifest does not match the bounded exact schema")
    version = data["version"]
    if not isinstance(version, str) or VERSION_PATTERN.fullmatch(version) is None:
        raise DesktopCLIError("manifest version is not a safe x.y.z value")
    expected_name = f"LingTai-{version}-macOS-universal.dmg"
    if data["file_name"] != expected_name or dmg.name != expected_name:
        raise DesktopCLIError("DMG does not have the exact deterministic file name")
    if data["architectures"] != list(ARCHITECTURES):
        raise DesktopCLIError("manifest architectures must be arm64+x86_64")
    if data["minimum_macos"] != MINIMUM_MACOS:
        raise DesktopCLIError("manifest minimum macOS must be 13.0")
    if type(data["size_bytes"]) is not int or data["size_bytes"] <= 0 or data["size_bytes"] != dmg_stat.st_size:
        raise DesktopCLIError("DMG size does not match manifest")
    sha = data["sha256"]
    if not isinstance(sha, str) or SHA_PATTERN.fullmatch(sha) is None or _sha256_file(dmg, "DMG") != sha:
        raise DesktopCLIError("DMG SHA-256 does not match manifest")
    git_sha, git_tree, dirty = data["packaging_git_sha"], data["packaging_git_tree"], data["packaging_git_dirty"]
    if not isinstance(git_sha, str) or GIT_SHA_PATTERN.fullmatch(git_sha) is None:
        raise DesktopCLIError("manifest packaging Git SHA is invalid")
    if not isinstance(git_tree, str) or GIT_SHA_PATTERN.fullmatch(git_tree) is None or type(dirty) is not bool:
        raise DesktopCLIError("manifest packaging Git facts are invalid")
    state = (data["signing"], data["notarization"])
    if state == (DIAGNOSTIC_SIGNING, DIAGNOSTIC_NOTARIZATION):
        diagnostic = True
        if not allow_diagnostic:
            raise DesktopCLIError("diagnostic artifact requires explicit --allow-diagnostic")
    elif state == (RELEASE_SIGNING, RELEASE_NOTARIZATION):
        diagnostic = False
        if dirty:
            raise DesktopCLIError("release-ready manifest requires clean packaging Git facts")
    else:
        raise DesktopCLIError("manifest has unknown or ambiguous signing/notarization state")
    return Manifest(version, expected_name, dmg_stat.st_size, sha, data["signing"],
                    data["notarization"], git_sha, git_tree, dirty,
                    _sha256_bytes(raw), diagnostic)


def _paths(home: Path | None) -> ManagedPaths:
    raw = Path(home) if home is not None else Path(os.environ.get("HOME", ""))
    if not os.fspath(raw) or not raw.is_absolute():
        raise DesktopCLIError("HOME must be present and absolute")
    try:
        facts = raw.lstat()
    except OSError as error:
        raise DesktopCLIError("HOME must be an existing real directory") from error
    if stat.S_ISLNK(facts.st_mode) or not stat.S_ISDIR(facts.st_mode):
        raise DesktopCLIError("HOME must be an existing real directory, not a symlink")
    local = raw / ".local"
    root = local / "share/lingtai-desktop"
    return ManagedPaths(raw, local, local / "bin", root, root / "cli",
                        root / "versions", root / "receipts", root / "current",
                        local / "bin/lingtai-desktop")


def _require_nonroot(effective_uid: int | None) -> None:
    uid = os.geteuid() if effective_uid is None else effective_uid
    if uid == 0:
        raise DesktopCLIError("mutating commands refuse effective uid 0")


def _ensure_directory(path: Path, mode: int, *, preserve_existing_mode: bool = False) -> None:
    created = False
    if path.exists() or path.is_symlink():
        facts = path.lstat()
        if stat.S_ISLNK(facts.st_mode) or not stat.S_ISDIR(facts.st_mode):
            raise DesktopCLIError(f"managed directory is not a real directory (symlink refused): {path.name}")
    else:
        path.mkdir(mode=mode)
        created = True
    if created or not preserve_existing_mode:
        os.chmod(path, mode, follow_symlinks=False)


def _require_real_directory(path: Path, label: str) -> None:
    try:
        facts = path.lstat()
    except OSError as error:
        raise DesktopCLIError(f"{label} is missing") from error
    if stat.S_ISLNK(facts.st_mode) or not stat.S_ISDIR(facts.st_mode):
        raise DesktopCLIError(f"{label} is not a real directory (symlink refused)")


def _prepare_layout(paths: ManagedPaths) -> None:
    _ensure_directory(paths.local, 0o700, preserve_existing_mode=True)
    _ensure_directory(paths.bin, 0o700, preserve_existing_mode=True)
    _ensure_directory(paths.local / "share", 0o700, preserve_existing_mode=True)
    _ensure_directory(paths.root, 0o700)
    _ensure_directory(paths.cli, 0o700)
    _ensure_directory(paths.versions, 0o700)
    _ensure_directory(paths.receipts, 0o700)


def _inspect_app(app: Path, version: str) -> Path:
    if app.is_symlink() or not app.is_dir() or app.name != APP_NAME:
        raise DesktopCLIError("staged App is not an exact real LingTai.app directory")
    _require_real_directory(app / "Contents", "App Contents")
    _require_real_directory(app / "Contents/MacOS", "App MacOS directory")
    plist_path = app / "Contents/Info.plist"
    raw = _read_bytes_nofollow(plist_path, "App Info.plist", 1024 * 1024)
    try:
        plist = plistlib.loads(raw)
    except plistlib.InvalidFileException as error:
        raise DesktopCLIError("App Info.plist is invalid") from error
    expected = {"CFBundleIdentifier": BUNDLE_ID, "CFBundleShortVersionString": version,
                "CFBundleVersion": version, "LSMinimumSystemVersion": MINIMUM_MACOS,
                "CFBundleExecutable": "LingTai"}
    if not isinstance(plist, dict) or any(plist.get(key) != value for key, value in expected.items()):
        raise DesktopCLIError("App bundle id/version/executable/minimum macOS facts are invalid")
    executable = app / "Contents/MacOS/LingTai"
    facts = _regular_nofollow(executable, "App executable")
    if not facts.st_mode & stat.S_IXUSR:
        raise DesktopCLIError("App executable is not executable")
    return executable


def bundle_tree_digest(app: Path) -> str:
    if app.is_symlink() or not app.is_dir():
        raise DesktopCLIError("bundle digest root must be a real directory")
    digest = hashlib.sha256()

    def visit(directory: Path) -> None:
        try:
            entries = sorted(os.scandir(directory), key=lambda entry: entry.name)
        except OSError as error:
            raise DesktopCLIError("could not traverse installed bundle") from error
        for entry in entries:
            path = Path(entry.path)
            relative = os.fspath(path.relative_to(app))
            facts = entry.stat(follow_symlinks=False)
            mode = stat.S_IMODE(facts.st_mode)
            if stat.S_ISDIR(facts.st_mode):
                kind, payload = "directory", b""
            elif stat.S_ISREG(facts.st_mode):
                kind, payload = "file", _read_bytes_nofollow(path, "bundle file")
            elif stat.S_ISLNK(facts.st_mode):
                kind = "symlink"
                target = os.readlink(path)
                if os.path.isabs(target):
                    raise DesktopCLIError("bundle contains an absolute symlink")
                normalized = os.path.normpath(os.path.join(os.path.dirname(relative), target))
                if normalized == ".." or normalized.startswith("../"):
                    raise DesktopCLIError("bundle symlink escapes the bundle")
                payload = os.fsencode(target)
            else:
                raise DesktopCLIError("bundle contains an unsupported filesystem object")
            digest.update(relative.encode("utf-8") + b"\0" + kind.encode() + b"\0" + f"{mode:o}".encode() + b"\0")
            digest.update(hashlib.sha256(payload).digest() if kind == "file" else payload)
            digest.update(b"\0")
            if kind == "directory":
                visit(path)

    visit(app)
    return digest.hexdigest()


def _receipt(manifest: Manifest, bundle_digest: str) -> dict[str, object]:
    return {
        "schema_version": RECEIPT_SCHEMA, "version": manifest.version,
        "bundle_id": BUNDLE_ID, "minimum_macos": MINIMUM_MACOS,
        "architectures": list(ARCHITECTURES),
        "source_dmg": {"file_name": manifest.file_name, "size_bytes": manifest.size_bytes, "sha256": manifest.sha256},
        "manifest_sha256": manifest.manifest_sha256, "signing": manifest.signing,
        "notarization": manifest.notarization, "packaging_git_sha": manifest.packaging_git_sha,
        "packaging_git_tree": manifest.packaging_git_tree,
        "packaging_git_dirty": manifest.packaging_git_dirty,
        "bundle_tree_sha256": bundle_digest,
        "classification": "diagnostic" if manifest.diagnostic else "release-ready",
        "managed_app_path": f"versions/{manifest.version}/{APP_NAME}",
    }


def _launcher_bytes(module_bytes: bytes | None = None, verifier_bytes: bytes | None = None) -> bytes:
    module_bytes = Path(__file__).read_bytes() if module_bytes is None else module_bytes
    if verifier_bytes is None:
        sibling = Path(__file__).parent / "verify-macos-package.py"
        source = sibling if sibling.is_file() else Path(__file__).parent / "scripts/verify-macos-package.py"
        verifier_bytes = _read_bytes_nofollow(source, "independent package verifier", 1024 * 1024)
    module_sha = _sha256_bytes(module_bytes)
    verifier_sha = _sha256_bytes(verifier_bytes)
    return (f"#!/usr/bin/env python3\n{LAUNCHER_MARKER}\n"
            "import hashlib, os, stat, sys\n"
            "sys.dont_write_bytecode=True\n"
            "root=os.path.join(os.environ.get('HOME',''),'.local','share','lingtai-desktop','cli')\n"
            f"expected={{'desktop_user_cli.py':'{module_sha}','verify-macos-package.py':'{verifier_sha}'}}\n"
            "for name,digest in expected.items():\n"
            " path=os.path.join(root,name); facts=os.lstat(path)\n"
            " if not stat.S_ISREG(facts.st_mode): raise SystemExit('lingtai-desktop: managed CLI integrity failure')\n"
            " fd=os.open(path,os.O_RDONLY|getattr(os,'O_NOFOLLOW',0))\n"
            " with os.fdopen(fd,'rb') as stream: actual=hashlib.sha256(stream.read()).hexdigest()\n"
            " if actual!=digest: raise SystemExit('lingtai-desktop: managed CLI integrity failure')\n"
            "sys.path.insert(0,root)\n"
            "from desktop_user_cli import installed_main\n"
            "raise SystemExit(installed_main())\n").encode()


def _identity(path: Path) -> tuple[int, int]:
    facts = path.lstat()
    return facts.st_dev, facts.st_ino


def _matches_identity(path: Path, expected: tuple[int, int]) -> bool:
    try:
        return _identity(path) == expected
    except OSError:
        return False


def _publish_file_exclusive(source: Path, destination: Path, mode: int) -> tuple[int, int]:
    try:
        os.chmod(source, mode, follow_symlinks=False)
        facts = _regular_nofollow(source, f"staged {destination.name}")
        if stat.S_IMODE(facts.st_mode) != mode:
            raise DesktopCLIError(f"staged {destination.name} has an incorrect mode")
        identity = (facts.st_dev, facts.st_ino)
        os.link(source, destination, follow_symlinks=False)
    except FileExistsError as error:
        raise DesktopCLIError(f"refusing to overwrite existing {destination.name}") from error
    except DesktopCLIError:
        raise
    except OSError as error:
        raise DesktopCLIError(f"could not publish {destination.name}") from error
    return identity


def _unlink_if_identity(path: Path, expected: tuple[int, int] | None) -> None:
    if expected is not None and _matches_identity(path, expected):
        path.unlink()


def _rename_exclusive(source: Path, destination: Path) -> None:
    if destination.exists() or destination.is_symlink():
        raise DesktopCLIError("version collision: managed version already exists")
    if sys.platform == "darwin":
        renamex_np = ctypes.CDLL(None, use_errno=True).renamex_np
        renamex_np.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
        renamex_np.restype = ctypes.c_int
        if renamex_np(os.fsencode(source), os.fsencode(destination), 0x00000004) != 0:  # RENAME_EXCL
            error_number = ctypes.get_errno()
            if error_number == 17:
                raise DesktopCLIError("version collision: managed version already exists")
            raise DesktopCLIError("could not exclusively publish managed version")
    else:
        os.rename(source, destination)


def _remove_owned_version(path: Path, expected_digest: str,
                          expected_identity: tuple[int, int] | None = None) -> None:
    if expected_identity is not None and not _matches_identity(path, expected_identity):
        raise DesktopCLIError("refusing to remove a replaced managed version")
    if path.is_symlink() or not path.is_dir() or bundle_tree_digest(path / APP_NAME) != expected_digest:
        raise DesktopCLIError("refusing to remove an unproven or tampered managed version")
    if sorted(item.name for item in path.iterdir()) != [APP_NAME]:
        raise DesktopCLIError("refusing to remove managed version with unknown files")
    shutil.rmtree(path)


def _owned_file_state(path: Path, expected: bytes, mode: int, label: str) -> bool:
    if not path.exists() and not path.is_symlink():
        return False
    if _read_bytes_nofollow(path, label, 1024 * 1024) != expected or stat.S_IMODE(path.stat().st_mode) != mode:
        raise DesktopCLIError(f"refusing to overwrite pre-existing unrelated {label}")
    return True


def _validate_owned_cli(paths: ManagedPaths) -> None:
    module = paths.cli / "desktop_user_cli.py"
    verifier = paths.cli / "verify-macos-package.py"
    module_bytes = _read_bytes_nofollow(module, "managed CLI", 1024 * 1024)
    verifier_bytes = _read_bytes_nofollow(verifier, "managed verifier", 1024 * 1024)
    _owned_file_state(module, module_bytes, 0o600, "managed CLI")
    _owned_file_state(verifier, verifier_bytes, 0o600, "managed verifier")
    _owned_file_state(paths.launcher, _launcher_bytes(module_bytes, verifier_bytes), 0o755, "launcher")


def _read_receipt(paths: ManagedPaths, version: str) -> dict[str, object]:
    if VERSION_PATTERN.fullmatch(version) is None:
        raise DesktopCLIError("version must be a safe x.y.z value")
    receipt_path = paths.receipts / f"{version}.json"
    raw = _read_bytes_nofollow(receipt_path, "receipt", 8192)
    try:
        receipt = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise DesktopCLIError("receipt is invalid") from error
    if not isinstance(receipt, dict) or set(receipt) != RECEIPT_KEYS or receipt.get("schema_version") != RECEIPT_SCHEMA:
        raise DesktopCLIError("receipt does not match the bounded exact schema")
    expected_path = f"versions/{version}/{APP_NAME}"
    if receipt.get("version") != version or receipt.get("managed_app_path") != expected_path:
        raise DesktopCLIError("receipt version/path relation is invalid")
    if receipt.get("bundle_id") != BUNDLE_ID or receipt.get("minimum_macos") != MINIMUM_MACOS or receipt.get("architectures") != list(ARCHITECTURES):
        raise DesktopCLIError("receipt App facts are invalid")
    if receipt.get("classification") not in {"diagnostic", "release-ready"}:
        raise DesktopCLIError("receipt classification is invalid")
    source = receipt.get("source_dmg")
    if not isinstance(source, dict) or set(source) != {"file_name", "size_bytes", "sha256"}:
        raise DesktopCLIError("receipt source artifact facts are invalid")
    if source.get("file_name") != f"LingTai-{version}-macOS-universal.dmg":
        raise DesktopCLIError("receipt source artifact name is invalid")
    if type(source.get("size_bytes")) is not int or source["size_bytes"] <= 0:
        raise DesktopCLIError("receipt source artifact size is invalid")
    if not isinstance(source.get("sha256"), str) or SHA_PATTERN.fullmatch(source["sha256"]) is None:
        raise DesktopCLIError("receipt source artifact SHA-256 is invalid")
    if not isinstance(receipt.get("manifest_sha256"), str) or SHA_PATTERN.fullmatch(receipt["manifest_sha256"]) is None:
        raise DesktopCLIError("receipt manifest SHA-256 is invalid")
    for field in ("packaging_git_sha", "packaging_git_tree"):
        if not isinstance(receipt.get(field), str) or GIT_SHA_PATTERN.fullmatch(receipt[field]) is None:
            raise DesktopCLIError("receipt packaging-only Git facts are invalid")
    if type(receipt.get("packaging_git_dirty")) is not bool:
        raise DesktopCLIError("receipt packaging-only dirty fact is invalid")
    expected_state = (
        (DIAGNOSTIC_SIGNING, DIAGNOSTIC_NOTARIZATION)
        if receipt["classification"] == "diagnostic"
        else (RELEASE_SIGNING, RELEASE_NOTARIZATION)
    )
    if (receipt.get("signing"), receipt.get("notarization")) != expected_state:
        raise DesktopCLIError("receipt signing/notarization state is inconsistent")
    digest = receipt.get("bundle_tree_sha256")
    if not isinstance(digest, str) or SHA_PATTERN.fullmatch(digest) is None:
        raise DesktopCLIError("receipt bundle digest is invalid")
    return receipt


def _active(paths: ManagedPaths) -> tuple[str, Path, dict[str, object]]:
    if not paths.current.is_symlink():
        if paths.current.exists():
            raise DesktopCLIError("managed current is not a symlink")
        raise DesktopCLIError("no current LingTai Desktop version is installed")
    for directory, label in ((paths.local, "managed .local"), (paths.bin, "managed bin"),
                             (paths.root, "managed root"), (paths.cli, "managed CLI directory"),
                             (paths.versions, "managed versions"), (paths.receipts, "managed receipts")):
        _require_real_directory(directory, label)
    target = os.readlink(paths.current)
    match = re.fullmatch(r"versions/([0-9]+\.[0-9]+\.[0-9]+)", target)
    if match is None:
        raise DesktopCLIError("managed current symlink escapes or is malformed")
    version = match.group(1)
    version_directory = paths.versions / version
    _require_real_directory(version_directory, "managed version")
    app = version_directory / APP_NAME
    receipt = _read_receipt(paths, version)
    _inspect_app(app, version)
    if bundle_tree_digest(app) != receipt["bundle_tree_sha256"]:
        raise DesktopCLIError("installed bundle digest does not match receipt")
    return version, app, receipt


def _preflight_uninstall(paths: ManagedPaths) -> UninstallPlan:
    for directory, label in (
        (paths.local, "managed .local"), (paths.bin, "managed bin"),
        (paths.local / "share", "managed share"), (paths.root, "managed root"),
        (paths.cli, "managed CLI directory"), (paths.versions, "managed versions"),
        (paths.receipts, "managed receipts"),
    ):
        _require_real_directory(directory, label)

    current_target: str | None = None
    current_identity: tuple[int, int] | None = None
    if paths.current.is_symlink():
        current_target = os.readlink(paths.current)
        current_identity = _identity(paths.current)
        if re.fullmatch(r"versions/[0-9]+\.[0-9]+\.[0-9]+", current_target) is None:
            raise DesktopCLIError("managed current symlink escapes or is malformed")
    elif paths.current.exists():
        raise DesktopCLIError("managed current is not a symlink")

    allowed_root = {"cli", "versions", "receipts"}
    if current_target is not None:
        allowed_root.add("current")
    actual_root = {path.name for path in paths.root.iterdir()}
    if actual_root != allowed_root:
        raise DesktopCLIError("managed root contains unknown or missing files")

    module = paths.cli / "desktop_user_cli.py"
    verifier = paths.cli / "verify-macos-package.py"
    if {path.name for path in paths.cli.iterdir()} != {module.name, verifier.name}:
        raise DesktopCLIError("managed CLI directory contains unknown files")
    _validate_owned_cli(paths)

    receipt_paths = sorted(paths.receipts.iterdir(), key=lambda path: path.name)
    version_paths = sorted(paths.versions.iterdir(), key=lambda path: path.name)
    receipt_by_version: dict[str, Path] = {}
    for receipt_path in receipt_paths:
        match = re.fullmatch(r"([0-9]+\.[0-9]+\.[0-9]+)\.json", receipt_path.name)
        facts = receipt_path.lstat()
        if match is None or stat.S_ISLNK(facts.st_mode) or not stat.S_ISREG(facts.st_mode):
            raise DesktopCLIError("receipts contains unknown files")
        receipt_by_version[match.group(1)] = receipt_path
    version_by_name: dict[str, Path] = {}
    for version_path in version_paths:
        if VERSION_PATTERN.fullmatch(version_path.name) is None:
            raise DesktopCLIError("versions contains unknown files")
        _require_real_directory(version_path, "managed version")
        version_by_name[version_path.name] = version_path
    if set(receipt_by_version) != set(version_by_name):
        raise DesktopCLIError("managed versions and receipts do not match")
    if current_target is not None and current_target.removeprefix("versions/") not in version_by_name:
        raise DesktopCLIError("managed current does not name an installed version")

    entries: list[UninstallEntry] = []
    for version in sorted(version_by_name, key=_version_tuple):
        receipt_path = receipt_by_version[version]
        version_path = version_by_name[version]
        receipt = _read_receipt(paths, version)
        if {path.name for path in version_path.iterdir()} != {APP_NAME}:
            raise DesktopCLIError("managed version contains unknown files")
        app = version_path / APP_NAME
        _inspect_app(app, version)
        if bundle_tree_digest(app) != receipt["bundle_tree_sha256"]:
            raise DesktopCLIError("installed bundle digest does not match receipt")
        entries.append(UninstallEntry(
            version, receipt, _identity(version_path), _identity(receipt_path)
        ))
    return UninstallPlan(
        tuple(entries), _identity(paths.root), _identity(paths.cli),
        _identity(paths.versions), _identity(paths.receipts), _identity(paths.launcher),
        _identity(module), _identity(verifier), current_target, current_identity,
    )


def _revalidate_uninstall_ancestors(paths: ManagedPaths, plan: UninstallPlan) -> None:
    for directory, identity, label in (
        (paths.root, plan.root_identity, "managed root"),
        (paths.cli, plan.cli_identity, "managed CLI directory"),
        (paths.versions, plan.versions_identity, "managed versions"),
        (paths.receipts, plan.receipts_identity, "managed receipts"),
    ):
        _require_real_directory(directory, label)
        if not _matches_identity(directory, identity):
            raise DesktopCLIError(f"refusing mutation through replaced {label}")


def _version_tuple(version: str) -> tuple[int, int, int]:
    if VERSION_PATTERN.fullmatch(version) is None:
        raise DesktopCLIError("version must be a safe x.y.z value")
    return tuple(int(value) for value in version.split("."))  # type: ignore[return-value]


def install(dmg: Path, manifest_path: Path, *, home: Path | None = None,
            allow_diagnostic: bool = False, platform: Platform | None = None,
            effective_uid: int | None = None, update: bool = False,
            repository_root: Path | None = None) -> str:
    _require_nonroot(effective_uid)
    paths = _paths(home)
    manifest = load_manifest(Path(dmg), Path(manifest_path), allow_diagnostic=allow_diagnostic)
    platform = platform or Platform()
    module_path = Path(__file__).resolve()
    repository_root = repository_root or (
        module_path.parent if module_path.parent.name == "cli" else module_path.parents[1]
    )
    if paths.root.exists() or paths.root.is_symlink():
        if paths.root.is_symlink() or not paths.root.is_dir():
            raise DesktopCLIError("managed root is a symlink or not a directory")
    current_version: str | None = None
    current_receipt: dict[str, object] | None = None
    if paths.current.exists() or paths.current.is_symlink():
        current_version, _, current_receipt = _active(paths)
    if update:
        if current_version is None:
            raise DesktopCLIError("update requires an installed current version")
        if _version_tuple(manifest.version) < _version_tuple(current_version):
            raise DesktopCLIError("update refuses lower versions")
        if manifest.version == current_version:
            assert current_receipt is not None
            source = current_receipt.get("source_dmg")
            identical = (
                isinstance(source, dict)
                and source == {"file_name": manifest.file_name, "size_bytes": manifest.size_bytes, "sha256": manifest.sha256}
                and current_receipt.get("manifest_sha256") == manifest.manifest_sha256
                and current_receipt.get("signing") == manifest.signing
                and current_receipt.get("notarization") == manifest.notarization
                and current_receipt.get("packaging_git_sha") == manifest.packaging_git_sha
                and current_receipt.get("packaging_git_tree") == manifest.packaging_git_tree
                and current_receipt.get("packaging_git_dirty") == manifest.packaging_git_dirty
                and current_receipt.get("classification") == ("diagnostic" if manifest.diagnostic else "release-ready")
            )
            if not identical:
                raise DesktopCLIError("same-version update requires an identical artifact and receipt")
            platform.verify_package(repository_root, Path(dmg), manifest.version, not manifest.diagnostic)
            return manifest.version
    elif current_version is not None:
        raise DesktopCLIError("an installation already exists; use update")
    platform.verify_package(repository_root, Path(dmg), manifest.version, not manifest.diagnostic)
    _prepare_layout(paths)
    final_version = paths.versions / manifest.version
    final_receipt = paths.receipts / f"{manifest.version}.json"
    if final_version.exists() or final_version.is_symlink() or final_receipt.exists() or final_receipt.is_symlink():
        raise DesktopCLIError("version collision: managed version already exists")
    managed_root_identity = _identity(paths.root)
    scratch = Path(tempfile.mkdtemp(prefix=".staging-", dir=paths.root))
    scratch_identity = _identity(scratch)
    receipt_identity: tuple[int, int] | None = None
    version_identity: tuple[int, int] | None = None
    cli_identity: tuple[int, int] | None = None
    verifier_identity: tuple[int, int] | None = None
    launcher_identity: tuple[int, int] | None = None
    digest = ""
    try:
        staged_version = scratch / "version"
        staged_version.mkdir(mode=0o700)
        staged_app = staged_version / APP_NAME
        with platform.mounted_app(Path(dmg), scratch) as source_app:
            platform.copy_app(source_app, staged_app)
        executable = _inspect_app(staged_app, manifest.version)
        fake_home, fake_tmp = scratch / "smoke-home", scratch / "smoke-tmp"
        fake_home.mkdir(mode=0o700)
        fake_tmp.mkdir(mode=0o700)
        platform.smoke(executable, fake_home, fake_tmp)
        digest = bundle_tree_digest(staged_app)
        receipt_bytes = (json.dumps(_receipt(manifest, digest), indent=2, sort_keys=True) + "\n").encode()
        staged_receipt = scratch / "receipt.json"
        staged_receipt.write_bytes(receipt_bytes)
        staged_receipt.chmod(0o600)
        if _FAILPOINT == "receipt":
            raise DesktopCLIError("injected receipt publication failure")
        receipt_identity = _publish_file_exclusive(staged_receipt, final_receipt, 0o600)
        staged_version_identity = _identity(staged_version)
        _rename_exclusive(staged_version, final_version)
        version_identity = staged_version_identity
        module_bytes = Path(__file__).read_bytes()
        verifier_source = (
            repository_root / "scripts/verify-macos-package.py"
            if (repository_root / "scripts/verify-macos-package.py").is_file()
            else repository_root / "verify-macos-package.py"
        )
        verifier_bytes = _read_bytes_nofollow(verifier_source, "independent package verifier", 1024 * 1024)
        launcher_bytes = _launcher_bytes(module_bytes, verifier_bytes)
        cli_path = paths.cli / "desktop_user_cli.py"
        verifier_path = paths.cli / "verify-macos-package.py"
        cli_exists = _owned_file_state(cli_path, module_bytes, 0o600, "managed CLI")
        verifier_exists = _owned_file_state(verifier_path, verifier_bytes, 0o600, "managed verifier")
        launcher_exists = _owned_file_state(paths.launcher, launcher_bytes, 0o755, "launcher")
        if _FAILPOINT == "launcher":
            raise DesktopCLIError("injected launcher publication failure")
        if not cli_exists:
            staged_cli = scratch / "desktop_user_cli.py"
            staged_cli.write_bytes(module_bytes)
            cli_identity = _publish_file_exclusive(staged_cli, cli_path, 0o600)
        if not verifier_exists:
            staged_verifier = scratch / "verify-macos-package.py"
            staged_verifier.write_bytes(verifier_bytes)
            verifier_identity = _publish_file_exclusive(staged_verifier, verifier_path, 0o600)
        if not launcher_exists:
            staged_launcher = scratch / "lingtai-desktop"
            staged_launcher.write_bytes(launcher_bytes)
            launcher_identity = _publish_file_exclusive(staged_launcher, paths.launcher, 0o755)
        old_target = os.readlink(paths.current) if paths.current.is_symlink() else None
        if paths.current.exists() and not paths.current.is_symlink():
            raise DesktopCLIError("managed current is not a symlink")
        temporary_current = paths.root / f".current-{uuid.uuid4().hex}"
        os.symlink(f"versions/{manifest.version}", temporary_current)
        try:
            if _FAILPOINT == "current":
                raise DesktopCLIError("injected current switch failure")
            os.replace(temporary_current, paths.current)
        finally:
            if temporary_current.is_symlink():
                temporary_current.unlink()
        if old_target is not None and os.readlink(paths.current) == old_target:
            raise DesktopCLIError("current switch did not take effect")
    except BaseException:
        _unlink_if_identity(paths.launcher, launcher_identity)
        if _matches_identity(paths.root, managed_root_identity):
            if 'cli_path' in locals():
                _unlink_if_identity(cli_path, cli_identity)
            if 'verifier_path' in locals():
                _unlink_if_identity(verifier_path, verifier_identity)
            if version_identity is not None and _matches_identity(final_version, version_identity):
                _remove_owned_version(final_version, digest, version_identity)
            _unlink_if_identity(final_receipt, receipt_identity)
        raise
    finally:
        if (_matches_identity(paths.root, managed_root_identity)
                and _matches_identity(scratch, scratch_identity)
                and not scratch.is_symlink()):
            shutil.rmtree(scratch)
    return manifest.version


def doctor(*, home: Path | None = None) -> tuple[str, dict[str, object]]:
    paths = _paths(home)
    for path in (paths.local, paths.bin, paths.root, paths.cli, paths.versions, paths.receipts):
        if path.is_symlink() or not path.is_dir():
            raise DesktopCLIError("managed layout contains a missing/non-directory/symlink root")
    _validate_owned_cli(paths)
    version, _, receipt = _active(paths)
    expected_states = ((DIAGNOSTIC_SIGNING, DIAGNOSTIC_NOTARIZATION, "diagnostic"),
                       (RELEASE_SIGNING, RELEASE_NOTARIZATION, "release-ready"))
    if (receipt["signing"], receipt["notarization"], receipt["classification"]) not in expected_states:
        raise DesktopCLIError("receipt artifact facts are inconsistent")
    source = receipt.get("source_dmg")
    if not isinstance(source, dict) or set(source) != {"file_name", "size_bytes", "sha256"}:
        raise DesktopCLIError("receipt source artifact facts are invalid")
    return version, receipt


def run_installed(arguments: Sequence[str], *, home: Path | None = None,
                  platform: Platform | None = None,
                  output: Callable[[str], None] = print) -> int:
    platform = platform or Platform()
    paths = _paths(home)
    command = "open" if not arguments else arguments[0]
    rest = list(arguments[1:])
    if command in {"open", "foreground", "version", "doctor"}:
        version, app, receipt = _active(paths)
        _validate_owned_cli(paths)
        if command == "open":
            if rest:
                raise DesktopCLIError("open takes no arguments")
            platform.open_app(app)
        elif command == "foreground":
            if rest[:1] == ["--"]:
                rest = rest[1:]
            platform.exec_app(app / "Contents/MacOS/LingTai", rest)
        elif command == "version":
            if rest:
                raise DesktopCLIError("version takes no arguments")
            output(f"version: {version}")
            output(f"artifact sha256: {receipt['source_dmg']['sha256']}")  # type: ignore[index]
            output(f"signing: {receipt['signing']}")
            output(f"notarization: {receipt['notarization']}")
            output("DIAGNOSTIC / NOT RELEASE READY" if receipt["classification"] == "diagnostic" else "RELEASE READY")
        else:
            if rest:
                raise DesktopCLIError("doctor takes no arguments")
            version, receipt = doctor(home=home)
            output(f"INTEGRITY PASS: managed LingTai Desktop {version}")
            output("NOT RELEASE READY: diagnostic developer preview" if receipt["classification"] == "diagnostic" else "RELEASE READY")
        return 0
    if command == "update":
        parser = argparse.ArgumentParser(prog="lingtai-desktop update")
        parser.add_argument("--dmg", required=True, type=Path)
        parser.add_argument("--manifest", required=True, type=Path)
        parser.add_argument("--allow-diagnostic", action="store_true")
        values = parser.parse_args(rest)
        version = install(values.dmg, values.manifest, home=home,
                          allow_diagnostic=values.allow_diagnostic, platform=platform, update=True)
        output(f"updated LingTai Desktop to {version}")
        return 0
    if command == "uninstall":
        parser = argparse.ArgumentParser(prog="lingtai-desktop uninstall")
        group = parser.add_mutually_exclusive_group(required=True)
        group.add_argument("--version")
        group.add_argument("--all", action="store_true")
        values = parser.parse_args(rest)
        uninstall_all(home=home) if values.all else uninstall_version(values.version, home=home)
        output("uninstalled managed LingTai Desktop files")
        return 0
    raise DesktopCLIError(f"unknown command: {command}; expected open, foreground, version, doctor, update, or uninstall")


def uninstall_version(version: str, *, home: Path | None = None,
                      effective_uid: int | None = None) -> None:
    _require_nonroot(effective_uid)
    paths = _paths(home)
    _version_tuple(version)
    plan = _preflight_uninstall(paths)
    entry = next((item for item in plan.entries if item.version == version), None)
    if entry is None:
        raise DesktopCLIError("requested managed version is not installed")
    is_current = plan.current_target == f"versions/{version}"
    version_path = paths.versions / version
    _revalidate_uninstall_ancestors(paths, plan)
    _remove_owned_version(version_path, entry.receipt["bundle_tree_sha256"], entry.version_identity)  # type: ignore[arg-type]
    receipt_path = paths.receipts / f"{version}.json"
    _revalidate_uninstall_ancestors(paths, plan)
    if not _matches_identity(receipt_path, entry.receipt_identity):
        raise DesktopCLIError("refusing to remove a replaced receipt")
    receipt_path.unlink()
    if is_current:
        _unlink_if_identity(paths.current, plan.current_identity)


def uninstall_all(*, home: Path | None = None, effective_uid: int | None = None) -> None:
    _require_nonroot(effective_uid)
    paths = _paths(home)
    plan = _preflight_uninstall(paths)
    module = paths.cli / "desktop_user_cli.py"
    verifier = paths.cli / "verify-macos-package.py"
    for entry in plan.entries:
        _revalidate_uninstall_ancestors(paths, plan)
        _remove_owned_version(
            paths.versions / entry.version,
            entry.receipt["bundle_tree_sha256"],  # type: ignore[arg-type]
            entry.version_identity,
        )
        receipt_path = paths.receipts / f"{entry.version}.json"
        _revalidate_uninstall_ancestors(paths, plan)
        if not _matches_identity(receipt_path, entry.receipt_identity):
            raise DesktopCLIError("refusing to remove a replaced receipt")
        receipt_path.unlink()
    _revalidate_uninstall_ancestors(paths, plan)
    _unlink_if_identity(paths.current, plan.current_identity)
    _unlink_if_identity(paths.launcher, plan.launcher_identity)
    _unlink_if_identity(module, plan.module_identity)
    _unlink_if_identity(verifier, plan.verifier_identity)
    for directory, identity in (
        (paths.cli, plan.cli_identity), (paths.receipts, plan.receipts_identity),
        (paths.versions, plan.versions_identity), (paths.root, plan.root_identity),
    ):
        if not _matches_identity(directory, identity):
            raise DesktopCLIError(f"refusing to remove replaced managed directory: {directory.name}")
        try:
            directory.rmdir()
        except OSError as error:
            raise DesktopCLIError(f"refusing to remove non-empty managed directory: {directory.name}") from error


def installed_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="lingtai-desktop",
        description="Launch or manage the user-level LingTai Desktop installation.")
    parser.add_argument("arguments", nargs=argparse.REMAINDER,
        help="open | foreground [-- APP_ARGS] | version | doctor | update ... | uninstall ...")
    return parser


def installed_main(argv: Sequence[str] | None = None) -> int:
    values = installed_parser().parse_args(argv)
    try:
        return run_installed(values.arguments)
    except DesktopCLIError as error:
        print(f"lingtai-desktop: {error}", file=sys.stderr)
        return 1


def bootstrap_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Install LingTai Desktop into the current user's managed layout.")
    parser.add_argument("--dmg", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--allow-diagnostic", action="store_true")
    return parser


def bootstrap_main(argv: Sequence[str] | None = None) -> int:
    values = bootstrap_parser().parse_args(argv)
    try:
        version = install(values.dmg, values.manifest, allow_diagnostic=values.allow_diagnostic)
        _, receipt = doctor()
    except DesktopCLIError as error:
        print(f"install-macos-app: {error}", file=sys.stderr)
        return 1
    print(f"installed LingTai Desktop {version}")
    if receipt["classification"] == "diagnostic":
        print("WARNING: diagnostic developer preview; NOT RELEASE READY")
    print("launcher: $HOME/.local/bin/lingtai-desktop (PATH was not modified)")
    return 0


if __name__ == "__main__":
    raise SystemExit(installed_main())
