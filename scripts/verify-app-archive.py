#!/usr/bin/env python3
"""Independent fail-closed verifier/extractor for a portable LingTai.app archive."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import plistlib
import posixpath
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path, PurePosixPath
from typing import Callable, Iterable, Sequence


APP_NAME = "LingTai.app"
BUNDLE_ID = "ai.lingtai.desktop"
EXECUTABLE_RELATIVE = "Contents/MacOS/LingTai"
MINIMUM_MACOS = "13.0"
REQUIRED_ARCHITECTURES = ("arm64", "x86_64")
ARTIFACT_KIND = "lingtai-portable-app-archive"
MANIFEST_SCHEMA = 1
VERSION_PATTERN = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")
SHA_PATTERN = re.compile(r"[0-9a-f]{64}")
GIT_PATTERN = re.compile(r"[0-9a-f]{40}")
MAX_MANIFEST_BYTES = 16 * 1024
# 512 MiB leaves more than 20x headroom over the current roughly 23 MiB archive.
MAX_ARCHIVE_BYTES = 512 * 1024 * 1024
MAX_MEMBERS = 100_000
MAX_FILE_BYTES = 2 * 1024 * 1024 * 1024
MAX_TOTAL_BYTES = 4 * 1024 * 1024 * 1024
MANIFEST_KEYS = {
    "architectures",
    "archive_file_name",
    "archive_sha256",
    "archive_size_bytes",
    "artifact_kind",
    "bundle_executable",
    "bundle_identifier",
    "bundle_name",
    "bundle_tree_sha256",
    "bundle_version",
    "executable_sha256",
    "executable_size_bytes",
    "minimum_macos",
    "packaging_git_dirty",
    "packaging_git_head",
    "packaging_git_tree",
    "schema_version",
    "version",
}


class VerificationError(RuntimeError):
    """A bounded verification failure safe to report to a terminal."""


def _regular_nofollow(path: Path, label: str) -> os.stat_result:
    try:
        facts = path.lstat()
    except OSError as error:
        raise VerificationError(f"{label} must be an existing regular file, not a symlink") from error
    if stat.S_ISLNK(facts.st_mode) or not stat.S_ISREG(facts.st_mode):
        raise VerificationError(f"{label} must be an existing regular file, not a symlink")
    return facts


def _open_regular_nofollow(path: Path, label: str) -> tuple[int, os.stat_result]:
    flags = (os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
             | getattr(os, "O_NONBLOCK", 0))
    descriptor: int | None = None
    try:
        descriptor = os.open(path, flags)
        facts = os.fstat(descriptor)
    except OSError as error:
        if descriptor is not None:
            try:
                os.close(descriptor)
            except OSError:
                pass
        raise VerificationError(
            f"{label} must be an existing regular file, not a symlink"
        ) from error
    if not stat.S_ISREG(facts.st_mode):
        os.close(descriptor)
        raise VerificationError(f"{label} must be an existing regular file, not a symlink")
    return descriptor, facts


def _read_nofollow(path: Path, label: str, limit: int) -> bytes:
    try:
        descriptor, facts = _open_regular_nofollow(path, label)
        with os.fdopen(descriptor, "rb") as stream:
            if facts.st_size > limit:
                raise VerificationError(f"{label} is too large")
            value = stream.read(limit + 1)
    except VerificationError:
        raise
    except OSError as error:
        raise VerificationError(f"could not read {label}") from error
    if len(value) > limit:
        raise VerificationError(f"{label} is too large")
    return value


def load_manifest(archive: Path, manifest: Path) -> tuple[dict[str, object], bytes]:
    archive = Path(archive)
    archive_descriptor, archive_facts = _open_regular_nofollow(archive, "App archive")
    os.close(archive_descriptor)
    if archive_facts.st_size > MAX_ARCHIVE_BYTES:
        raise VerificationError("App archive is too large")
    raw = _read_nofollow(Path(manifest), "manifest", MAX_MANIFEST_BYTES)
    try:
        data = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise VerificationError("manifest must be bounded valid JSON") from error
    if not isinstance(data, dict) or set(data) != MANIFEST_KEYS:
        raise VerificationError("manifest does not match the bounded exact schema")
    if type(data["schema_version"]) is not int or data["schema_version"] != MANIFEST_SCHEMA:
        raise VerificationError("manifest schema version is invalid")
    version = data["version"]
    if not isinstance(version, str) or VERSION_PATTERN.fullmatch(version) is None:
        raise VerificationError("manifest version is not a safe x.y.z value")
    expected_archive = f"LingTai-{version}-macOS-universal.app.tar.gz"
    if data["archive_file_name"] != expected_archive or archive.name != expected_archive:
        raise VerificationError("archive does not have the exact deterministic file name")
    exact = {
        "artifact_kind": ARTIFACT_KIND,
        "schema_version": MANIFEST_SCHEMA,
        "bundle_name": APP_NAME,
        "bundle_identifier": BUNDLE_ID,
        "bundle_version": version,
        "bundle_executable": EXECUTABLE_RELATIVE,
        "minimum_macos": MINIMUM_MACOS,
        "architectures": list(REQUIRED_ARCHITECTURES),
    }
    if any(data[key] != value for key, value in exact.items()):
        raise VerificationError("manifest artifact or App identity facts are invalid")
    for field in ("archive_sha256", "executable_sha256", "bundle_tree_sha256"):
        if not isinstance(data[field], str) or SHA_PATTERN.fullmatch(data[field]) is None:
            raise VerificationError(f"manifest {field} is invalid")
    for field in ("packaging_git_head", "packaging_git_tree"):
        if not isinstance(data[field], str) or GIT_PATTERN.fullmatch(data[field]) is None:
            raise VerificationError("manifest packaging Git provenance is invalid")
    if type(data["packaging_git_dirty"]) is not bool:
        raise VerificationError("manifest packaging dirty fact is invalid")
    for field in ("archive_size_bytes", "executable_size_bytes"):
        if type(data[field]) is not int or data[field] <= 0:
            raise VerificationError(f"manifest {field} is invalid")
    if data["archive_size_bytes"] != archive_facts.st_size:
        raise VerificationError("archive size does not match manifest")
    return data, raw


def _canonical_member_name(member: tarfile.TarInfo) -> str:
    raw = member.name
    if not isinstance(raw, str) or not raw or "\0" in raw or "\\" in raw or raw.startswith("/"):
        raise VerificationError("archive contains an absolute or invalid member name")
    try:
        raw.encode("utf-8")
    except UnicodeEncodeError as error:
        raise VerificationError("archive contains an invalid member name") from error
    if raw.endswith("/"):
        if not member.isdir():
            raise VerificationError("archive member has a conflicting trailing slash")
        raw = raw[:-1]
    parts = raw.split("/")
    if any(part in {"", ".", ".."} for part in parts):
        raise VerificationError("archive contains path traversal or an invalid member name")
    if parts[0] != APP_NAME:
        raise VerificationError("archive contains extra top-level content")
    return "/".join(parts)


def _symlink_target(member_name: str, target: str) -> str:
    if not isinstance(target, str) or not target or "\0" in target or "\\" in target or target.startswith("/"):
        raise VerificationError("archive contains an absolute or invalid symlink target")
    normalized = posixpath.normpath(posixpath.join(posixpath.dirname(member_name), target))
    if normalized != APP_NAME and not normalized.startswith(APP_NAME + "/"):
        raise VerificationError("archive symlink target escapes LingTai.app")
    return normalized


def _hardlink_target(target: str) -> str:
    if not isinstance(target, str) or not target or "\0" in target or "\\" in target or target.startswith("/"):
        raise VerificationError("archive contains an absolute or invalid hardlink target")
    parts = target.rstrip("/").split("/")
    if any(part in {"", ".", ".."} for part in parts) or parts[0] != APP_NAME:
        raise VerificationError("archive hardlink target escapes LingTai.app")
    return "/".join(parts)


def _preflight_members(members: Iterable[tarfile.TarInfo]) -> tuple[dict[str, tarfile.TarInfo], dict[str, str]]:
    by_name: dict[str, tarfile.TarInfo] = {}
    link_targets: dict[str, str] = {}
    total_size = 0
    member_count = 0
    for member in members:
        member_count += 1
        if member_count > MAX_MEMBERS:
            raise VerificationError("archive has an invalid or excessive member count")
        name = _canonical_member_name(member)
        if name in by_name:
            raise VerificationError("archive contains duplicate or conflicting members")
        if member.mode & ~0o777:
            raise VerificationError("archive member has unsupported permission bits")
        if member.isfile():
            if member.size < 0 or member.size > MAX_FILE_BYTES:
                raise VerificationError("archive member size is invalid or excessive")
            total_size += member.size
        elif member.isdir():
            if member.size not in (0,):
                raise VerificationError("archive directory has conflicting data")
            if (member.mode & 0o700) != 0o700:
                raise VerificationError("archive directory lacks required owner permissions")
        elif member.issym():
            if member.size != 0:
                raise VerificationError("archive link has conflicting data")
            link_targets[name] = _symlink_target(name, member.linkname)
        elif member.islnk():
            if member.size != 0:
                raise VerificationError("archive link has conflicting data")
            link_targets[name] = _hardlink_target(member.linkname)
        else:
            raise VerificationError("archive contains a device, FIFO, socket, or unsupported member")
        if total_size > MAX_TOTAL_BYTES:
            raise VerificationError("archive expands beyond the bounded size")
        by_name[name] = member
    if member_count == 0:
        raise VerificationError("archive has an invalid or excessive member count")
    root = by_name.get(APP_NAME)
    if root is None or not root.isdir():
        raise VerificationError("archive must contain one exact top-level LingTai.app directory")
    for name, member in by_name.items():
        if name == APP_NAME:
            continue
        parent = str(PurePosixPath(name).parent)
        parent_member = by_name.get(parent)
        if parent_member is None or not parent_member.isdir():
            raise VerificationError("archive member has a missing or non-directory parent")
        if member.islnk():
            target_member = by_name.get(link_targets[name])
            if target_member is None or not target_member.isfile():
                raise VerificationError("archive hardlink target is absent or not a regular file")
    return by_name, link_targets


def _apply_symlink_mode(path: Path, member: tarfile.TarInfo) -> None:
    parent_descriptor: int | None = None
    try:
        flags = (os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
                 | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0))
        parent_descriptor = os.open(path.parent, flags)
        before = os.stat(path.name, dir_fd=parent_descriptor, follow_symlinks=False)
        if (not stat.S_ISLNK(before.st_mode)
                or os.readlink(path.name, dir_fd=parent_descriptor) != member.linkname):
            raise VerificationError("archive symlink identity changed before mode restoration")
        identity = before.st_dev, before.st_ino
        expected_mode = member.mode & 0o777
        os.chmod(
            path.name,
            expected_mode,
            dir_fd=parent_descriptor,
            follow_symlinks=False,
        )
        after = os.stat(path.name, dir_fd=parent_descriptor, follow_symlinks=False)
        if ((after.st_dev, after.st_ino) != identity
                or not stat.S_ISLNK(after.st_mode)
                or stat.S_IMODE(after.st_mode) != expected_mode
                or os.readlink(path.name, dir_fd=parent_descriptor) != member.linkname):
            raise VerificationError("archive symlink identity changed during mode restoration")
    except VerificationError:
        raise
    except (OSError, NotImplementedError, ValueError) as error:
        raise VerificationError("could not apply archive symlink mode") from error
    finally:
        if parent_descriptor is not None:
            try:
                os.close(parent_descriptor)
            except OSError:
                pass


def _extract_members(archive: tarfile.TarFile, destination: Path,
                     by_name: dict[str, tarfile.TarInfo], link_targets: dict[str, str]) -> None:
    directories = sorted(
        ((name, member) for name, member in by_name.items() if member.isdir()),
        key=lambda pair: (pair[0].count("/"), pair[0]),
    )
    for name, _ in directories:
        path = destination / name
        try:
            path.mkdir(mode=0o700)
        except OSError as error:
            raise VerificationError("could not create private archive directory") from error
    expected_members = iter(by_name.items())
    for member in archive:
        try:
            name, expected = next(expected_members)
        except StopIteration as error:
            raise VerificationError("archive changed after member preflight") from error
        if (_canonical_member_name(member) != name
                or (member.type, member.mode, member.size, member.linkname)
                != (expected.type, expected.mode, expected.size, expected.linkname)):
            raise VerificationError("archive changed after member preflight")
        if not member.isfile():
            continue
        path = destination / name
        source = archive.extractfile(member)
        if source is None:
            raise VerificationError("could not read regular archive member")
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
        try:
            descriptor = os.open(path, flags, 0o600)
            with os.fdopen(descriptor, "wb") as output, source:
                shutil.copyfileobj(source, output, length=1024 * 1024)
            if path.stat().st_size != member.size:
                raise VerificationError("extracted member size changed")
            os.chmod(path, member.mode & 0o777, follow_symlinks=False)
        except VerificationError:
            raise
        except OSError as error:
            raise VerificationError("could not extract regular archive member") from error
    try:
        next(expected_members)
    except StopIteration:
        pass
    else:
        raise VerificationError("archive changed after member preflight")
    for name, member in sorted(by_name.items()):
        if member.issym():
            path = destination / name
            try:
                os.symlink(member.linkname, path)
            except OSError as error:
                raise VerificationError("could not create archive symlink") from error
            _apply_symlink_mode(path, member)
    for name, member in sorted(by_name.items()):
        if member.islnk():
            try:
                os.link(destination / link_targets[name], destination / name, follow_symlinks=False)
            except OSError as error:
                raise VerificationError("could not create archive hardlink") from error
    for name, member in sorted(directories, key=lambda pair: (-pair[0].count("/"), pair[0])):
        try:
            os.chmod(destination / name, member.mode & 0o777, follow_symlinks=False)
        except OSError as error:
            raise VerificationError("could not apply archive directory mode") from error


def _bundle_tree_digest(app: Path) -> str:
    if app.is_symlink() or not app.is_dir():
        raise VerificationError("extracted App root is not a real directory")
    digest = hashlib.sha256()

    def visit(directory: Path) -> None:
        try:
            entries = sorted(os.scandir(directory), key=lambda entry: entry.name)
        except OSError as error:
            raise VerificationError("could not traverse extracted App") from error
        for entry in entries:
            path = Path(entry.path)
            relative = os.fspath(path.relative_to(app))
            facts = entry.stat(follow_symlinks=False)
            mode = stat.S_IMODE(facts.st_mode)
            if stat.S_ISDIR(facts.st_mode):
                kind, payload = "directory", b""
            elif stat.S_ISREG(facts.st_mode):
                kind, payload = "file", bytes.fromhex(_sha256_file(path, "bundle file"))
            elif stat.S_ISLNK(facts.st_mode):
                kind = "symlink"
                target = os.readlink(path)
                normalized = os.path.normpath(os.path.join(os.path.dirname(relative), target))
                if os.path.isabs(target) or normalized == ".." or normalized.startswith("../"):
                    raise VerificationError("extracted App symlink escapes the bundle")
                payload = os.fsencode(target)
            else:
                raise VerificationError("extracted App contains an unsupported filesystem object")
            digest.update(relative.encode() + b"\0" + kind.encode() + b"\0" + f"{mode:o}".encode() + b"\0")
            digest.update(payload)
            digest.update(b"\0")
            if kind == "directory":
                visit(path)

    visit(app)
    return digest.hexdigest()


def _default_architecture_reader(executable: Path) -> tuple[str, ...]:
    try:
        result = subprocess.run(
            ["/usr/bin/lipo", "-archs", executable],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
    except OSError as error:
        raise VerificationError("lipo architecture check could not start") from error
    if result.returncode:
        raise VerificationError("lipo architecture check failed")
    return tuple(sorted(set(result.stdout.split())))


def _sha256_file(path: Path, label: str) -> str:
    try:
        descriptor, _ = _open_regular_nofollow(path, label)
        digest = hashlib.sha256()
        with os.fdopen(descriptor, "rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except VerificationError:
        raise
    except OSError as error:
        raise VerificationError(f"could not hash {label}") from error
    return digest.hexdigest()


def _verify_app(app: Path, manifest: dict[str, object],
                architecture_reader: Callable[[Path], tuple[str, ...]]) -> None:
    plist_path = app / "Contents/Info.plist"
    raw = _read_nofollow(plist_path, "App Info.plist", 1024 * 1024)
    try:
        plist = plistlib.loads(raw)
    except plistlib.InvalidFileException as error:
        raise VerificationError("App Info.plist is invalid") from error
    expected = {
        "CFBundleIdentifier": BUNDLE_ID,
        "CFBundleShortVersionString": manifest["version"],
        "CFBundleVersion": manifest["version"],
        "CFBundleExecutable": "LingTai",
        "LSMinimumSystemVersion": MINIMUM_MACOS,
    }
    if not isinstance(plist, dict) or any(plist.get(key) != value for key, value in expected.items()):
        raise VerificationError("App bundle identity/version/executable facts do not match manifest")
    executable = app / EXECUTABLE_RELATIVE
    facts = _regular_nofollow(executable, "App executable")
    if not facts.st_mode & stat.S_IXUSR:
        raise VerificationError("App executable is not executable")
    executable_digest = _sha256_file(executable, "App executable")
    if facts.st_size != manifest["executable_size_bytes"] or executable_digest != manifest["executable_sha256"]:
        raise VerificationError("App executable size or SHA-256 does not match manifest")
    if architecture_reader(executable) != REQUIRED_ARCHITECTURES:
        raise VerificationError("App executable architectures do not match manifest")
    if _bundle_tree_digest(app) != manifest["bundle_tree_sha256"]:
        raise VerificationError("App recursive bundle digest does not match manifest")


def _extract_verified(archive_path: Path, manifest: dict[str, object], destination: Path,
                      architecture_reader: Callable[[Path], tuple[str, ...]]) -> None:
    try:
        descriptor, facts = _open_regular_nofollow(archive_path, "App archive")
        with os.fdopen(descriptor, "rb") as stream:
            if facts.st_size > MAX_ARCHIVE_BYTES:
                raise VerificationError("App archive is too large")
            digest = hashlib.sha256()
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
            if facts.st_size != manifest["archive_size_bytes"] or digest.hexdigest() != manifest["archive_sha256"]:
                raise VerificationError("archive size or SHA-256 does not match manifest")
            stream.seek(0)
            try:
                with tarfile.open(fileobj=stream, mode="r|gz") as tar:
                    by_name, links = _preflight_members(tar)
                stream.seek(0)
                with tarfile.open(fileobj=stream, mode="r|gz") as tar:
                    _extract_members(tar, destination, by_name, links)
            except (tarfile.TarError, EOFError, OSError) as error:
                raise VerificationError("archive is malformed or truncated") from error
    except VerificationError:
        raise
    except OSError as error:
        raise VerificationError("could not read App archive") from error
    _verify_app(destination / APP_NAME, manifest, architecture_reader)


def verify_pair(
    archive: Path,
    manifest: Path,
    *,
    extract_to: Path | None = None,
    architecture_reader: Callable[[Path], tuple[str, ...]] | None = None,
) -> None:
    archive = Path(archive)
    data, _ = load_manifest(archive, Path(manifest))
    reader = architecture_reader or _default_architecture_reader
    if extract_to is None:
        with tempfile.TemporaryDirectory(prefix=".lingtai-app-verify-") as temporary:
            _extract_verified(archive, data, Path(temporary), reader)
        return
    destination = Path(extract_to)
    if destination.exists() or destination.is_symlink():
        raise VerificationError("extraction destination already exists")
    parent = destination.parent
    if parent.is_symlink() or not parent.is_dir():
        raise VerificationError("extraction parent must be a real directory")
    destination.mkdir(mode=0o700)
    identity = (destination.lstat().st_dev, destination.lstat().st_ino)
    try:
        _extract_verified(archive, data, destination, reader)
    except BaseException:
        try:
            facts = destination.lstat()
            if (facts.st_dev, facts.st_ino) == identity and stat.S_ISDIR(facts.st_mode):
                shutil.rmtree(destination)
        except FileNotFoundError:
            pass
        raise


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Independently verify a LingTai portable App archive.")
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--extract-to", type=Path)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        values = build_parser().parse_args(argv)
        verify_pair(values.archive, values.manifest, extract_to=values.extract_to)
    except VerificationError as error:
        print(f"verify-app-archive: {error}", file=sys.stderr)
        return 1
    print("App archive verification: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
