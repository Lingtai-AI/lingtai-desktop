#!/usr/bin/env python3
"""Produce a portable LingTai.app tar.gz and exact manifest without a DMG."""

from __future__ import annotations

import argparse
import dataclasses
import gzip
import hashlib
import json
import os
import plistlib
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path
from typing import Callable, Sequence


APP_NAME = "LingTai.app"
BUNDLE_ID = "ai.lingtai.desktop"
EXECUTABLE_RELATIVE = "Contents/MacOS/LingTai"
MINIMUM_MACOS = "13.0"
REQUIRED_ARCHITECTURES = ("arm64", "x86_64")
ARTIFACT_KIND = "lingtai-portable-app-archive"
MANIFEST_SCHEMA = 1
VERSION_PATTERN = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")


class ArchivePackagingError(RuntimeError):
    """A bounded local packaging failure."""


@dataclasses.dataclass(frozen=True)
class ArchiveNames:
    archive: str
    manifest: str


@dataclasses.dataclass(frozen=True)
class PackagingGitFacts:
    head: str
    tree: str
    dirty: bool


@dataclasses.dataclass(frozen=True)
class AppFacts:
    version: str
    executable_size_bytes: int
    executable_sha256: str
    architectures: tuple[str, ...]
    bundle_tree_sha256: str


def archive_names(version: str) -> ArchiveNames:
    if VERSION_PATTERN.fullmatch(version) is None:
        raise ArchivePackagingError("unsafe version; expected three numeric components")
    stem = f"LingTai-{version}-macOS-universal.app"
    return ArchiveNames(f"{stem}.tar.gz", f"{stem}.manifest.json")


def _run(arguments: Sequence[os.PathLike[str] | str], label: str) -> str:
    try:
        result = subprocess.run(
            [os.fspath(value) for value in arguments],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
    except OSError as error:
        raise ArchivePackagingError(f"{label} could not start") from error
    if result.returncode:
        output = (result.stdout or "").replace("\0", "").strip()[-1200:]
        raise ArchivePackagingError(
            f"{label} failed (exit {result.returncode})" + (f": {output}" if output else "")
        )
    return result.stdout or ""


def _regular_nofollow(path: Path, label: str) -> os.stat_result:
    try:
        facts = path.lstat()
    except OSError as error:
        raise ArchivePackagingError(f"{label} is missing") from error
    if stat.S_ISLNK(facts.st_mode) or not stat.S_ISREG(facts.st_mode):
        raise ArchivePackagingError(f"{label} must be a regular file, not a symlink")
    return facts


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _bundle_tree_digest(app: Path) -> str:
    if app.is_symlink() or not app.is_dir():
        raise ArchivePackagingError("bundle digest root must be a real directory")
    digest = hashlib.sha256()

    def visit(directory: Path) -> None:
        try:
            entries = sorted(os.scandir(directory), key=lambda entry: entry.name)
        except OSError as error:
            raise ArchivePackagingError("could not traverse input App") from error
        for entry in entries:
            path = Path(entry.path)
            relative = os.fspath(path.relative_to(app))
            facts = entry.stat(follow_symlinks=False)
            mode = stat.S_IMODE(facts.st_mode)
            if stat.S_ISDIR(facts.st_mode):
                kind, payload = "directory", b""
            elif stat.S_ISREG(facts.st_mode):
                kind = "file"
                payload = bytes.fromhex(_sha256_file(path))
            elif stat.S_ISLNK(facts.st_mode):
                kind = "symlink"
                target = os.readlink(path)
                if os.path.isabs(target):
                    raise ArchivePackagingError("input App contains an absolute symlink")
                normalized = os.path.normpath(os.path.join(os.path.dirname(relative), target))
                if normalized == ".." or normalized.startswith("../"):
                    raise ArchivePackagingError("input App symlink escapes the bundle")
                payload = os.fsencode(target)
            else:
                raise ArchivePackagingError("input App contains an unsupported filesystem object")
            digest.update(
                relative.encode("utf-8")
                + b"\0"
                + kind.encode()
                + b"\0"
                + f"{mode:o}".encode()
                + b"\0"
            )
            digest.update(payload)
            digest.update(b"\0")
            if kind == "directory":
                visit(path)

    visit(app)
    return digest.hexdigest()


def _default_architecture_reader(executable: Path) -> tuple[str, ...]:
    lipo = Path("/usr/bin/lipo")
    if not lipo.is_file():
        raise ArchivePackagingError("/usr/bin/lipo is unavailable")
    values = tuple(sorted(set(_run([lipo, "-archs", executable], "lipo architecture check").split())))
    if not values:
        raise ArchivePackagingError("lipo returned no architectures")
    return values


def inspect_app(
    app: Path,
    *,
    architecture_reader: Callable[[Path], tuple[str, ...]] | None = None,
) -> AppFacts:
    app = Path(app)
    if app.name != APP_NAME or app.is_symlink() or not app.is_dir():
        raise ArchivePackagingError("--app must name a real LingTai.app directory, not a symlink")
    plist_path = app / "Contents/Info.plist"
    _regular_nofollow(plist_path, "App Info.plist")
    try:
        with plist_path.open("rb") as stream:
            plist = plistlib.load(stream)
    except (OSError, plistlib.InvalidFileException) as error:
        raise ArchivePackagingError("input App has no valid Contents/Info.plist") from error
    version = plist.get("CFBundleShortVersionString") if isinstance(plist, dict) else None
    expected = {
        "CFBundleIdentifier": BUNDLE_ID,
        "CFBundleVersion": version,
        "CFBundleExecutable": "LingTai",
        "LSMinimumSystemVersion": MINIMUM_MACOS,
    }
    if (
        not isinstance(version, str)
        or VERSION_PATTERN.fullmatch(version) is None
        or any(plist.get(key) != value for key, value in expected.items())
    ):
        raise ArchivePackagingError("input App bundle identity/version/executable facts are invalid")
    executable = app / EXECUTABLE_RELATIVE
    executable_facts = _regular_nofollow(executable, "App executable")
    if not executable_facts.st_mode & stat.S_IXUSR:
        raise ArchivePackagingError("App executable is not executable")
    architectures = (architecture_reader or _default_architecture_reader)(executable)
    if architectures != REQUIRED_ARCHITECTURES:
        raise ArchivePackagingError("App executable must contain arm64 and x86_64 only")
    return AppFacts(
        version,
        executable_facts.st_size,
        _sha256_file(executable),
        architectures,
        _bundle_tree_digest(app),
    )


def packaging_git_facts(repository_root: Path) -> PackagingGitFacts:
    head = _run(["git", "-C", repository_root, "rev-parse", "HEAD"], "packaging Git HEAD check").strip()
    tree = _run(["git", "-C", repository_root, "rev-parse", "HEAD^{tree}"], "packaging Git tree check").strip()
    status_output = _run(
        ["git", "-C", repository_root, "status", "--porcelain=v1", "--untracked-files=no"],
        "packaging Git tracked-diff check",
    )
    if re.fullmatch(r"[0-9a-f]{40}", head) is None or re.fullmatch(r"[0-9a-f]{40}", tree) is None:
        raise ArchivePackagingError("packaging Git provenance is unavailable")
    return PackagingGitFacts(head, tree, bool(status_output.strip()))


def render_manifest(names: ArchiveNames, facts: AppFacts, git: PackagingGitFacts,
                    archive: Path) -> bytes:
    payload = {
        "architectures": list(facts.architectures),
        "archive_file_name": names.archive,
        "archive_sha256": _sha256_file(archive),
        "archive_size_bytes": archive.stat().st_size,
        "artifact_kind": ARTIFACT_KIND,
        "bundle_executable": EXECUTABLE_RELATIVE,
        "bundle_identifier": BUNDLE_ID,
        "bundle_name": APP_NAME,
        "bundle_tree_sha256": facts.bundle_tree_sha256,
        "bundle_version": facts.version,
        "executable_sha256": facts.executable_sha256,
        "executable_size_bytes": facts.executable_size_bytes,
        "minimum_macos": MINIMUM_MACOS,
        "packaging_git_dirty": git.dirty,
        "packaging_git_head": git.head,
        "packaging_git_tree": git.tree,
        "schema_version": MANIFEST_SCHEMA,
        "version": facts.version,
    }
    return (json.dumps(payload, indent=2, sort_keys=True) + "\n").encode("utf-8")


def _write_archive(app: Path, destination: Path, source_date_epoch: int) -> None:
    def normalized(info: tarfile.TarInfo) -> tarfile.TarInfo:
        info.uid = 0
        info.gid = 0
        info.uname = ""
        info.gname = ""
        info.mtime = source_date_epoch
        info.pax_headers = {}
        return info

    with destination.open("xb") as raw:
        with gzip.GzipFile(filename="", fileobj=raw, mode="wb", compresslevel=9,
                           mtime=source_date_epoch) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT,
                              dereference=False) as archive:
                archive.add(app, arcname=APP_NAME, recursive=True, filter=normalized)


def _identity(path: Path) -> tuple[int, int]:
    facts = path.lstat()
    return facts.st_dev, facts.st_ino


def _rollback_if_owned(path: Path, identity: tuple[int, int]) -> None:
    try:
        if _identity(path) == identity:
            path.unlink()
    except FileNotFoundError:
        return


def _matches_identity(path: Path, identity: tuple[int, int]) -> bool:
    try:
        return _identity(path) == identity
    except OSError:
        return False


def publish_pair(staged_archive: Path, staged_manifest: Path,
                 final_archive: Path, final_manifest: Path) -> None:
    archive_identity = _identity(staged_archive)
    manifest_identity = _identity(staged_manifest)
    try:
        os.link(staged_archive, final_archive, follow_symlinks=False)
    except FileExistsError as error:
        raise ArchivePackagingError("archive appeared during packaging; refusing overwrite") from error
    except OSError as error:
        raise ArchivePackagingError("artifact publication failed while creating archive") from error
    try:
        os.link(staged_manifest, final_manifest, follow_symlinks=False)
    except FileExistsError as error:
        _rollback_if_owned(final_archive, archive_identity)
        raise ArchivePackagingError("manifest appeared during packaging; refusing overwrite") from error
    except OSError as error:
        _rollback_if_owned(final_archive, archive_identity)
        raise ArchivePackagingError("artifact publication failed while creating manifest") from error
    if (not _matches_identity(final_archive, archive_identity)
            or not _matches_identity(final_manifest, manifest_identity)):
        _rollback_if_owned(final_archive, archive_identity)
        _rollback_if_owned(final_manifest, manifest_identity)
        raise ArchivePackagingError("artifact pair changed during publication")


def _default_verifier_runner(archive: Path, manifest: Path) -> None:
    verifier = Path(__file__).with_name("verify-app-archive.py")
    _run(
        [sys.executable, verifier, "--archive", archive, "--manifest", manifest],
        "independent App-archive verification",
    )


def package_app_archive(
    app: Path,
    output_directory: Path,
    *,
    repository_root: Path,
    source_date_epoch: int = 0,
    architecture_reader: Callable[[Path], tuple[str, ...]] | None = None,
    verifier_runner: Callable[[Path, Path], None] | None = None,
) -> tuple[Path, Path]:
    app = Path(app).resolve(strict=False)
    facts = inspect_app(app, architecture_reader=architecture_reader)
    before_digest = facts.bundle_tree_sha256
    git = packaging_git_facts(Path(repository_root))
    requested_output = Path(output_directory)
    if requested_output.is_symlink():
        raise ArchivePackagingError("--output-dir must be a real directory, not a symlink")
    output = requested_output.resolve(strict=False)
    try:
        output.relative_to(app)
    except ValueError:
        pass
    else:
        raise ArchivePackagingError("output directory must not be inside the input App")
    output.mkdir(parents=True, exist_ok=True)
    if requested_output.is_symlink() or not output.is_dir():
        raise ArchivePackagingError("--output-dir must be a real directory, not a symlink")
    names = archive_names(facts.version)
    final_archive, final_manifest = output / names.archive, output / names.manifest
    for destination in (final_archive, final_manifest):
        if destination.exists() or destination.is_symlink():
            raise ArchivePackagingError(f"refusing to overwrite existing artifact: {destination.name}")

    with tempfile.TemporaryDirectory(prefix=".lingtai-app-archive-", dir=output) as temporary:
        scratch = Path(temporary)
        staged_archive = scratch / names.archive
        staged_manifest = scratch / names.manifest
        _write_archive(app, staged_archive, source_date_epoch)
        staged_manifest.write_bytes(render_manifest(names, facts, git, staged_archive))
        (verifier_runner or _default_verifier_runner)(staged_archive, staged_manifest)
        if _bundle_tree_digest(app) != before_digest:
            raise ArchivePackagingError("input App changed during packaging; artifact is untrusted")
        final_git = packaging_git_facts(Path(repository_root))
        if final_git != git:
            raise ArchivePackagingError("packaging Git provenance changed during packaging")
        publish_pair(staged_archive, staged_manifest, final_archive, final_manifest)
    return final_archive, final_manifest


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Package a verified universal LingTai.app as a portable tar.gz plus manifest."
    )
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--source-date-epoch", type=int, default=0)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        values = build_parser().parse_args(argv)
        archive, manifest = package_app_archive(
            values.app,
            values.output_dir,
            repository_root=Path(__file__).resolve().parents[1],
            source_date_epoch=values.source_date_epoch,
        )
    except ArchivePackagingError as error:
        print(f"package-app-archive: {error}", file=sys.stderr)
        return 1
    print(f"archive: {archive}")
    print(f"manifest: {manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
