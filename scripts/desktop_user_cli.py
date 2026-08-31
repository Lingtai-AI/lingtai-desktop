#!/usr/bin/env python3
"""Fail-closed user-level installer and launcher for LingTai Desktop on macOS."""

from __future__ import annotations

import argparse
import contextlib
import ctypes
import dataclasses
import hashlib
import http.client
import json
import os
import plistlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import urllib.parse
import uuid
from pathlib import Path
from typing import Callable, Mapping, Sequence


APP_NAME = "LingTai.app"
BUNDLE_ID = "ai.lingtai.desktop"
MINIMUM_MACOS = "13.0"
ARCHITECTURES = ("arm64", "x86_64")
MAX_VERSION_COMPONENT_DIGITS = 9
MAX_VERSION_LENGTH = 3 * MAX_VERSION_COMPONENT_DIGITS + 2
VERSION_PATTERN = re.compile(
    rf"[0-9]{{1,{MAX_VERSION_COMPONENT_DIGITS}}}\."
    rf"[0-9]{{1,{MAX_VERSION_COMPONENT_DIGITS}}}\."
    rf"[0-9]{{1,{MAX_VERSION_COMPONENT_DIGITS}}}"
)
SHA_PATTERN = re.compile(r"[0-9a-f]{64}")
GIT_SHA_PATTERN = re.compile(r"[0-9a-f]{40}")
RECEIPT_SCHEMA = 2
LAUNCHER_MARKER = "# lingtai-desktop-support-bootstrap-v1"
SUPPORT_REEXEC_MARKER = "LINGTAI_DESKTOP_SUPPORT_REEXEC"
STABLE_BOOTSTRAP_SHA256 = "6c246f7af6602eeee0d697bcd5c830029939bd786ba3ecbf3cf8c41846ac02e6"
ARTIFACT_KIND = "lingtai-portable-app-archive"
MANIFEST_SCHEMA = 1
EXECUTABLE_RELATIVE = "Contents/MacOS/LingTai"
# 512 MiB leaves more than 20x headroom over the current roughly 23 MiB archive.
MAX_ARCHIVE_BYTES = 512 * 1024 * 1024
MAX_MANIFEST_BYTES = 16 * 1024
MAX_RELEASE_METADATA_BYTES = 64 * 1024
OFFICIAL_REPOSITORY = "Lingtai-AI/lingtai-desktop"
OFFICIAL_LATEST_RELEASE_URL = (
    "https://api.github.com/repos/Lingtai-AI/lingtai-desktop/releases/latest"
)
OFFICIAL_RELEASE_TAG_URL = (
    "https://api.github.com/repos/Lingtai-AI/lingtai-desktop/releases/tags/v{}"
)
OFFICIAL_HTTPS_HOSTS = frozenset({
    "api.github.com", "github.com", "release-assets.githubusercontent.com",
})
RELEASE_USER_AGENT = "lingtai-desktop/official-release-downloader"
RELEASE_ACCEPT = "application/vnd.github+json"
EXPLICIT_RELEASE_TIMEOUT = 15.0
MAX_REDIRECTS = 5
UPDATE_CACHE_SCHEMA = 1
UPDATE_CACHE_KEYS = {"schema_version", "checked_at", "latest_version"}
MAX_UPDATE_CACHE_BYTES = 512
DEFAULT_UPDATE_CHECK_INTERVAL = 24 * 60 * 60
AUTOMATIC_RELEASE_TIMEOUT = 2.0
STAGED_APP_SMOKE_TIMEOUT = 60
SUPPORT_UPDATE_CACHE_SCHEMA = "lingtai.desktop.support-update-check/v1"
SUPPORT_UPDATE_CACHE_KEYS = {
    "schema", "checked_at", "latest_support_version", "release_tag",
    "generation_id", "manifest_sha256", "declined",
}
MAX_SUPPORT_UPDATE_CACHE_BYTES = 2048
DEFAULT_SUPPORT_UPDATE_CHECK_INTERVAL = 24 * 60 * 60
_SUPPORT_REEXEC_CONSUMED = False  # Set only by the stable bootstrap after import.
MANIFEST_KEYS = {
    "architectures", "archive_file_name", "archive_sha256",
    "archive_size_bytes", "artifact_kind", "bundle_executable",
    "bundle_identifier", "bundle_name", "bundle_tree_sha256",
    "bundle_version", "executable_sha256", "executable_size_bytes",
    "minimum_macos", "packaging_git_dirty", "packaging_git_head",
    "packaging_git_tree", "schema_version", "version",
}
RECEIPT_KEYS = {
    "schema_version", "artifact_kind", "version", "bundle_id",
    "bundle_version", "bundle_executable", "minimum_macos",
    "architectures", "source_archive", "manifest_sha256",
    "packaging_git_head", "packaging_git_tree", "packaging_git_dirty",
    "executable_size_bytes", "executable_sha256", "bundle_tree_sha256",
    "managed_app_path",
}
_FAILPOINT: str | None = None  # Tests inject failures without production flags.

SUPPORT_MANIFEST_SCHEMA = "lingtai.desktop.support/v1"
SUPPORT_STATE_SCHEMA = "lingtai.desktop.support-state/v1"
SUPPORT_PENDING_SCHEMA = "lingtai.desktop.support-pending/v1"
SUPPORT_BOOTSTRAP_PROTOCOL = 1
SUPPORT_REPOSITORY = OFFICIAL_REPOSITORY
SUPPORT_MANIFEST_NAME = "support-manifest.json"
SUPPORT_PAYLOAD_NAMES = ("desktop_user_cli.py", "verify-app-archive.py")
SUPPORT_PAYLOAD_MODE = 0o600
SUPPORT_GENERATION_MODE = 0o700
SUPPORT_GENERATION_DIGEST_LENGTH = 12
MAX_SUPPORT_MANIFEST_BYTES = 16 * 1024
MAX_SUPPORT_STATE_BYTES = 16 * 1024
MAX_SUPPORT_PENDING_BYTES = 4 * 1024
MAX_SUPPORT_PAYLOAD_BYTES = 2 * 1024 * 1024
MAX_FAILED_SUPPORT_GENERATIONS = 32
SUPPORT_GENERATION_PATTERN = re.compile(
    rf"([0-9]{{1,{MAX_VERSION_COMPONENT_DIGITS}}}\."
    rf"[0-9]{{1,{MAX_VERSION_COMPONENT_DIGITS}}}\."
    rf"[0-9]{{1,{MAX_VERSION_COMPONENT_DIGITS}}})-"
    rf"([0-9a-f]{{{SUPPORT_GENERATION_DIGEST_LENGTH}}})"
)
SUPPORT_MANIFEST_KEYS = {
    "schema", "support_version", "generation_id", "release_tag",
    "repository", "bootstrap_protocol", "minimum_bootstrap_protocol", "files",
}
SUPPORT_FILE_KEYS = {"name", "size", "mode", "sha256"}
SUPPORT_STATE_KEYS = {
    "schema", "high_water_version", "high_water_manifest_sha256",
    "last_good_generation", "failed_generations",
}
SUPPORT_FAILED_KEYS = {"generation_id", "manifest_sha256"}
SUPPORT_PENDING_KEYS = {
    "schema", "from_generation", "to_generation", "to_manifest_sha256",
    "expected_current_dev", "expected_current_ino", "requested_argv_sha256",
    "explicit_retry", "rollback_pointer_name",
}
SUPPORT_ROLLBACK_POINTER_PATTERN = re.compile(r"\.rollback-[0-9a-f]{32}")
INITIAL_INSTALL_SCHEMA = "lingtai.desktop.initial-install/v1"
INITIAL_INSTALL_NAME = "initial-install.json"
MAX_INITIAL_INSTALL_BYTES = 2048
INITIAL_INSTALL_KEYS = {
    "schema", "nonce", "app_version", "app_manifest_sha256",
    "support_generation", "support_manifest_sha256",
}


class DesktopCLIError(RuntimeError):
    """A bounded installer/launcher failure safe to print to a terminal."""


class InjectedInitialInstallCrash(BaseException):
    """Test-only process-death analogue that deliberately bypasses rollback."""


def _trip_initial(boundary: str) -> None:
    if _FAILPOINT == f"crash:{boundary}":
        raise InjectedInitialInstallCrash(boundary)


def _is_safe_version(value: object) -> bool:
    return (isinstance(value, str) and len(value) <= MAX_VERSION_LENGTH
            and value.isascii() and VERSION_PATTERN.fullmatch(value) is not None)


@dataclasses.dataclass(frozen=True)
class Manifest:
    version: str
    file_name: str
    size_bytes: int
    sha256: str
    executable_size_bytes: int
    executable_sha256: str
    bundle_tree_sha256: str
    packaging_git_head: str
    packaging_git_tree: str
    packaging_git_dirty: bool
    manifest_sha256: str


@dataclasses.dataclass(frozen=True)
class ReleaseAsset:
    name: str
    url: str
    size_bytes: int


@dataclasses.dataclass(frozen=True)
class OfficialRelease:
    version: str
    archive: ReleaseAsset
    manifest: ReleaseAsset


@dataclasses.dataclass(frozen=True)
class UpdateCheck:
    checked_at: int
    latest_version: str


@dataclasses.dataclass(frozen=True)
class SupportUpdateCheck:
    checked_at: int
    latest_support_version: str
    release_tag: str
    generation_id: str
    manifest_sha256: str
    declined: bool


@dataclasses.dataclass(frozen=True)
class SupportPayload:
    name: str
    size: int
    mode: int
    sha256: str


@dataclasses.dataclass(frozen=True)
class SupportManifest:
    support_version: str
    generation_id: str
    release_tag: str
    repository: str
    bootstrap_protocol: int
    minimum_bootstrap_protocol: int
    files: tuple[SupportPayload, ...]
    manifest_sha256: str


@dataclasses.dataclass(frozen=True)
class OfficialSupportRelease:
    version: str
    manifest: SupportManifest
    manifest_asset: ReleaseAsset
    payload_assets: Mapping[str, ReleaseAsset]
    manifest_bytes: bytes


@dataclasses.dataclass(frozen=True)
class FailedSupportGeneration:
    generation_id: str
    manifest_sha256: str


@dataclasses.dataclass(frozen=True)
class SupportState:
    high_water_version: str
    high_water_manifest_sha256: str
    last_good_generation: str
    failed_generations: tuple[FailedSupportGeneration, ...]


@dataclasses.dataclass(frozen=True)
class SupportPending:
    from_generation: str
    to_generation: str
    to_manifest_sha256: str
    expected_current_dev: int
    expected_current_ino: int
    requested_argv_sha256: str
    explicit_retry: bool
    rollback_pointer_name: str


@dataclasses.dataclass(frozen=True)
class InitialInstallJournal:
    nonce: str
    app_version: str
    app_manifest_sha256: str
    support_generation: str
    support_manifest_sha256: str


@dataclasses.dataclass(frozen=True)
class ValidatedSupportGeneration:
    path: Path
    manifest: SupportManifest
    directory_identity: tuple[int, int]
    file_identities: tuple[tuple[str, tuple[int, int]], ...]


@dataclasses.dataclass(frozen=True)
class ManagedPaths:
    home: Path
    local: Path
    bin: Path
    root: Path
    support: Path
    support_versions: Path
    support_current: Path
    support_pending: Path
    support_state: Path
    support_update_cache: Path
    versions: Path
    receipts: Path
    current: Path
    update_cache: Path
    initial_install: Path
    launcher: Path


@dataclasses.dataclass(frozen=True)
class InitialSupportPublication:
    generation: ValidatedSupportGeneration
    state_identity: tuple[int, int]
    current_identity: tuple[int, int]
    launcher_identity: tuple[int, int]
    generation_created: bool
    state_created: bool
    current_created: bool
    launcher_created: bool


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
    versions_identity: tuple[int, int]
    receipts_identity: tuple[int, int]
    update_cache_identity: tuple[int, int] | None
    current_target: str | None
    current_identity: tuple[int, int] | None


@dataclasses.dataclass(frozen=True)
class SupportUninstallPlan:
    support_identity: tuple[int, int]
    versions_identity: tuple[int, int]
    generations: tuple[ValidatedSupportGeneration, ...]
    current_target: str
    current_identity: tuple[int, int]
    state_identity: tuple[int, int]
    pending_identity: tuple[int, int] | None
    rollback_pointer_name: str | None
    rollback_pointer_identity: tuple[int, int] | None
    update_cache_identity: tuple[int, int] | None
    launcher_identity: tuple[int, int]


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

    def _archive_verifier(self, repository_root: Path) -> Path:
        verifier = repository_root / "scripts/verify-app-archive.py"
        if not verifier.is_file():
            verifier = repository_root / "verify-app-archive.py"
        _regular_nofollow(verifier, "independent package verifier")
        return verifier

    def verify_archive(self, repository_root: Path, archive: Path, manifest: Path) -> None:
        _run([
            sys.executable, self._archive_verifier(repository_root),
            "--archive", archive, "--manifest", manifest,
        ], label="independent App-archive verification")

    def verify_and_extract_archive(self, repository_root: Path, archive: Path,
                                   manifest: Path, destination: Path) -> Path:
        _run([
            sys.executable, self._archive_verifier(repository_root),
            "--archive", archive, "--manifest", manifest,
            "--extract-to", destination,
        ], label="independent App-archive extraction")
        return destination / APP_NAME

    def smoke(self, executable: Path, fake_home: Path, fake_tmp: Path) -> None:
        environment = {
            "HOME": os.fspath(fake_home), "TMPDIR": os.fspath(fake_tmp),
            "PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "LANG": "en_US.UTF-8",
            "LC_ALL": "en_US.UTF-8",
        }
        output = _run([executable, "--smoke"], label="staged App smoke",
                      environment=environment, timeout=STAGED_APP_SMOKE_TIMEOUT)
        markers = ("LINGTAI_NATIVE_SHELL_READY", "LINGTAI_LIB_UI_FULL_TARGET_SMOKE_OK")
        positions = [output.find(marker) for marker in markers]
        if any(value < 0 for value in positions) or positions != sorted(positions):
            raise DesktopCLIError("staged App smoke markers are absent or out of order")

    def open_app(self, app: Path) -> None:
        _run(["/usr/bin/open", app], label="LingTai Desktop launch")

    def exec_app(self, executable: Path, arguments: list[str]) -> None:
        os.execv(executable, [os.fspath(executable), *arguments])


class _HTTPSResponse:
    def __init__(self, response: http.client.HTTPResponse,
                 connection: http.client.HTTPSConnection) -> None:
        self.status = response.status
        self.headers = response.headers
        self._response = response
        self._connection = connection

    def read(self, size: int = -1) -> bytes:
        return self._response.read(size)

    def close(self) -> None:
        try:
            self._response.close()
        finally:
            self._connection.close()


class ReleaseTransport:
    """Injectable one-request HTTPS boundary; redirect policy stays above it."""

    def open(self, url: str, headers: dict[str, str], timeout: float) -> _HTTPSResponse:
        parts = urllib.parse.urlsplit(url)
        connection = http.client.HTTPSConnection(parts.hostname, timeout=timeout)
        target = urllib.parse.urlunsplit(("", "", parts.path or "/", parts.query, ""))
        try:
            connection.request("GET", target, headers=headers)
            return _HTTPSResponse(connection.getresponse(), connection)
        except (OSError, TimeoutError, http.client.HTTPException) as error:
            connection.close()
            raise DesktopCLIError("official release request failed") from error


def _validate_official_url(url: str, label: str) -> urllib.parse.SplitResult:
    if not isinstance(url, str) or not url.isascii():
        raise DesktopCLIError(f"{label} URL is malformed")
    try:
        parts = urllib.parse.urlsplit(url)
        port = parts.port
    except (TypeError, ValueError) as error:
        raise DesktopCLIError(f"{label} URL is malformed") from error
    if (parts.scheme != "https" or not parts.hostname
            or parts.username is not None or parts.password is not None
            or port is not None or parts.fragment
            or parts.hostname.lower() not in OFFICIAL_HTTPS_HOSTS):
        raise DesktopCLIError(f"{label} URL is not an allowed official HTTPS destination")
    return parts


def _response_header(response: object, name: str) -> str | None:
    headers = getattr(response, "headers", None)
    value = headers.get(name) if headers is not None else None
    return value if isinstance(value, str) else None


def _content_length(response: object, maximum_bytes: int,
                    expected_bytes: int | None = None) -> int | None:
    raw = _response_header(response, "Content-Length")
    if raw is None:
        return None
    if not raw.isascii() or not raw.isdecimal():
        raise DesktopCLIError("official release response Content-Length is invalid")
    length = int(raw)
    if length > maximum_bytes:
        raise DesktopCLIError("official release response is too large")
    if expected_bytes is not None and length != expected_bytes:
        raise DesktopCLIError("official release response length does not match asset metadata")
    return length


def _remaining_official_timeout(timeout: float, deadline: float | None) -> float:
    if deadline is None:
        return timeout
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise DesktopCLIError("official release metadata check timed out")
    return min(timeout, remaining)


def _open_official_response(url: str, transport: ReleaseTransport,
                            timeout: float, deadline: float | None = None) -> object:
    current = url
    headers = {"User-Agent": RELEASE_USER_AGENT, "Accept": RELEASE_ACCEPT}
    for redirect_count in range(MAX_REDIRECTS + 1):
        _validate_official_url(current, "official release")
        try:
            response = transport.open(
                current, headers, _remaining_official_timeout(timeout, deadline),
            )
        except DesktopCLIError:
            raise
        except (OSError, TimeoutError, http.client.HTTPException) as error:
            raise DesktopCLIError("official release request failed") from error
        try:
            _remaining_official_timeout(timeout, deadline)
        except DesktopCLIError:
            response.close()
            raise
        status = getattr(response, "status", None)
        if status in {301, 302, 303, 307, 308}:
            location = _response_header(response, "Location")
            response.close()
            if location is None or not location:
                raise DesktopCLIError("official release redirect is malformed")
            if redirect_count == MAX_REDIRECTS:
                raise DesktopCLIError("official release redirected too many times")
            try:
                current = urllib.parse.urljoin(current, location)
            except ValueError as error:
                raise DesktopCLIError("official release redirect is malformed") from error
            _validate_official_url(current, "official release redirect")
            continue
        if status != 200:
            response.close()
            raise DesktopCLIError("official release request returned a non-success status")
        return response
    raise DesktopCLIError("official release redirected too many times")


def _read_official_bytes(url: str, transport: ReleaseTransport,
                         maximum_bytes: int, timeout: float,
                         deadline: float | None = None,
                         expected_bytes: int | None = None) -> bytes:
    response = _open_official_response(url, transport, timeout, deadline)
    try:
        advertised = _content_length(response, maximum_bytes, expected_bytes)
        result = bytearray()
        while True:
            _remaining_official_timeout(timeout, deadline)
            block = response.read(min(64 * 1024, maximum_bytes + 1 - len(result)))
            _remaining_official_timeout(timeout, deadline)
            if not block:
                break
            result.extend(block)
            if len(result) > maximum_bytes:
                raise DesktopCLIError("official release response is too large")
        if advertised is not None and len(result) != advertised:
            raise DesktopCLIError("official release response was truncated")
        if expected_bytes is not None and len(result) != expected_bytes:
            raise DesktopCLIError("official release response length does not match asset metadata")
        return bytes(result)
    except DesktopCLIError:
        raise
    except (OSError, TimeoutError, http.client.HTTPException) as error:
        raise DesktopCLIError("official release response failed while streaming") from error
    finally:
        response.close()


def _asset_from_metadata(value: object, expected_name: str,
                         version: str, maximum_bytes: int) -> ReleaseAsset | None:
    if not isinstance(value, dict):
        raise DesktopCLIError("official release asset metadata is invalid")
    name, url, size_bytes = value.get("name"), value.get("browser_download_url"), value.get("size")
    if name != expected_name:
        return None
    if (not isinstance(url, str) or type(size_bytes) is not int
            or size_bytes <= 0 or size_bytes > maximum_bytes):
        raise DesktopCLIError("official release asset metadata is invalid")
    parts = _validate_official_url(url, "official release asset")
    owner, repository = OFFICIAL_REPOSITORY.split("/", 1)
    path_segments = parts.path.split("/")
    exact_tail = ["releases", "download", f"v{version}", expected_name]
    if (parts.hostname.lower() != "github.com" or parts.query
            or len(path_segments) != 7 or path_segments[0]
            or path_segments[1].lower() != owner.lower()
            or path_segments[2].lower() != repository.lower()
            or path_segments[3:] != exact_tail):
        raise DesktopCLIError("official release asset URL does not match its tag and name")
    return ReleaseAsset(expected_name, url, size_bytes)


def _exact_json_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON object key")
        result[key] = value
    return result


def discover_official_release(version: str | None = None, *,
                              transport: ReleaseTransport | None = None,
                              timeout: float = EXPLICIT_RELEASE_TIMEOUT,
                              deadline: float | None = None) -> OfficialRelease:
    if version is not None and not _is_safe_version(version):
        raise DesktopCLIError("release version must be a safe x.y.z value")
    transport = transport or ReleaseTransport()
    url = OFFICIAL_LATEST_RELEASE_URL if version is None else OFFICIAL_RELEASE_TAG_URL.format(version)
    raw = _read_official_bytes(
        url, transport, MAX_RELEASE_METADATA_BYTES, timeout, deadline,
    )
    try:
        metadata = json.loads(raw, object_pairs_hook=_exact_json_object)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError, ValueError) as error:
        raise DesktopCLIError("official release metadata is not bounded valid JSON") from error
    if not isinstance(metadata, dict):
        raise DesktopCLIError("official release metadata must be an object")
    tag = metadata.get("tag_name")
    draft, prerelease, assets = metadata.get("draft"), metadata.get("prerelease"), metadata.get("assets")
    if (not isinstance(tag, str) or not tag.startswith("v")
            or not _is_safe_version(tag[1:])
            or type(draft) is not bool or type(prerelease) is not bool
            or draft or prerelease or not isinstance(assets, list)):
        raise DesktopCLIError("official release metadata is not a stable release")
    discovered_version = tag[1:]
    if version is not None and discovered_version != version:
        raise DesktopCLIError("official release tag does not match the requested version")
    archive_name = f"LingTai-{discovered_version}-macOS-universal.app.tar.gz"
    manifest_name = f"LingTai-{discovered_version}-macOS-universal.app.manifest.json"
    archives = [asset for value in assets
                if (asset := _asset_from_metadata(value, archive_name, discovered_version,
                                                  MAX_ARCHIVE_BYTES)) is not None]
    manifests = [asset for value in assets
                 if (asset := _asset_from_metadata(value, manifest_name, discovered_version,
                                                   MAX_MANIFEST_BYTES)) is not None]
    if len(archives) != 1 or len(manifests) != 1:
        raise DesktopCLIError("official release must contain one exact archive and manifest asset")
    return OfficialRelease(discovered_version, archives[0], manifests[0])


def _support_asset_from_metadata(
        value: object, expected_names: Sequence[str], version: str) -> ReleaseAsset | None:
    if not isinstance(value, dict):
        raise DesktopCLIError("official release asset metadata is invalid")
    name = value.get("name")
    if not isinstance(name, str) or not name.isascii():
        raise DesktopCLIError("official release asset metadata is invalid")
    folded = {candidate.casefold(): candidate for candidate in expected_names}
    path_tail = name.replace("\\", "/").rsplit("/", 1)[-1]
    if ((name.casefold() in folded and name != folded[name.casefold()])
            or (path_tail.casefold() in folded and name != folded[path_tail.casefold()])):
        raise DesktopCLIError("official support release asset name is ambiguous")
    if name not in expected_names:
        return None
    maximum = (MAX_SUPPORT_MANIFEST_BYTES if name == SUPPORT_MANIFEST_NAME
               else MAX_SUPPORT_PAYLOAD_BYTES)
    return _asset_from_metadata(value, name, version, maximum)


def discover_official_support_release(
        version: str | None = None, *, transport: ReleaseTransport | None = None,
        timeout: float = EXPLICIT_RELEASE_TIMEOUT,
        deadline: float | None = None) -> OfficialSupportRelease:
    """Discover and authenticate official support metadata without publishing state.

    Authenticity means the TLS-protected official GitHub route plus the exact
    repository/tag/name metadata and manifest SHA-256 declarations. It is not a
    code-signing or release-signing claim.
    """
    if version is not None and not _is_safe_version(version):
        raise DesktopCLIError("support release version must be a safe x.y.z value")
    transport = transport or ReleaseTransport()
    url = OFFICIAL_LATEST_RELEASE_URL if version is None else OFFICIAL_RELEASE_TAG_URL.format(version)
    raw = _read_official_bytes(
        url, transport, MAX_RELEASE_METADATA_BYTES, timeout, deadline,
    )
    try:
        metadata = json.loads(raw, object_pairs_hook=_exact_json_object)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError, ValueError) as error:
        raise DesktopCLIError("official support release metadata is not bounded valid JSON") from error
    if not isinstance(metadata, dict):
        raise DesktopCLIError("official support release metadata must be an object")
    tag = metadata.get("tag_name")
    draft = metadata.get("draft")
    prerelease = metadata.get("prerelease")
    assets = metadata.get("assets")
    if (not isinstance(tag, str) or not tag.startswith("v")
            or not _is_safe_version(tag[1:]) or type(draft) is not bool
            or type(prerelease) is not bool or draft or prerelease
            or not isinstance(assets, list)):
        raise DesktopCLIError("official support release metadata is not a stable release")
    discovered = tag[1:]
    if version is not None and discovered != version:
        raise DesktopCLIError("official support release tag does not match requested version")
    expected = (SUPPORT_MANIFEST_NAME, *SUPPORT_PAYLOAD_NAMES)
    selected: dict[str, list[ReleaseAsset]] = {name: [] for name in expected}
    for value in assets:
        asset = _support_asset_from_metadata(value, expected, discovered)
        if asset is not None:
            selected[asset.name].append(asset)
    if any(len(selected[name]) != 1 for name in expected):
        raise DesktopCLIError("official release must contain one exact support manifest and payload asset set")
    manifest_asset = selected[SUPPORT_MANIFEST_NAME][0]
    manifest_bytes = _read_official_bytes(
        manifest_asset.url, transport, MAX_SUPPORT_MANIFEST_BYTES, timeout,
        deadline, manifest_asset.size_bytes,
    )
    manifest = parse_support_manifest(manifest_bytes)
    if manifest.support_version != discovered or manifest.release_tag != tag:
        raise DesktopCLIError("official support manifest does not bind discovered release tag")
    payload_assets = {name: selected[name][0] for name in SUPPORT_PAYLOAD_NAMES}
    for declared in manifest.files:
        if payload_assets[declared.name].size_bytes != declared.size:
            raise DesktopCLIError("official support asset size does not match manifest")
    return OfficialSupportRelease(
        discovered, manifest, manifest_asset, payload_assets, manifest_bytes,
    )


def _download_official_asset(asset: ReleaseAsset, destination: Path,
                             transport: ReleaseTransport, maximum_bytes: int,
                             timeout: float, deadline: float | None = None) -> None:
    response = _open_official_response(asset.url, transport, timeout, deadline)
    descriptor: int | None = None
    try:
        advertised = _content_length(response, maximum_bytes, asset.size_bytes)
        flags = (os.O_WRONLY | os.O_CREAT | os.O_EXCL
                 | getattr(os, "O_NOFOLLOW", 0))
        descriptor = os.open(destination, flags, 0o600)
        written = 0
        with os.fdopen(descriptor, "wb") as stream:
            descriptor = None
            while True:
                _remaining_official_timeout(timeout, deadline)
                block = response.read(min(1024 * 1024, maximum_bytes + 1 - written))
                _remaining_official_timeout(timeout, deadline)
                if not block:
                    break
                written += len(block)
                if written > maximum_bytes or written > asset.size_bytes:
                    raise DesktopCLIError("official release asset exceeded its declared size")
                stream.write(block)
        if written != asset.size_bytes or (advertised is not None and written != advertised):
            raise DesktopCLIError("official release asset was truncated")
        facts = _regular_nofollow(destination, f"downloaded {asset.name}")
        if (facts.st_size != asset.size_bytes or stat.S_IMODE(facts.st_mode) != 0o600
                or facts.st_uid != os.geteuid() or facts.st_nlink != 1):
            raise DesktopCLIError("downloaded official release asset facts are invalid")
    except DesktopCLIError:
        raise
    except (OSError, TimeoutError, http.client.HTTPException) as error:
        raise DesktopCLIError("official release asset failed while streaming") from error
    finally:
        if descriptor is not None:
            os.close(descriptor)
        response.close()


@contextlib.contextmanager
def downloaded_official_release(version: str | None = None, *,
                                transport: ReleaseTransport | None = None,
                                timeout: float = EXPLICIT_RELEASE_TIMEOUT):
    transport = transport or ReleaseTransport()
    release = discover_official_release(version, transport=transport, timeout=timeout)
    try:
        scratch = Path(tempfile.mkdtemp(prefix="lingtai-desktop-download-"))
    except OSError as error:
        raise DesktopCLIError(
            "official release temporary directory could not be created"
        ) from error
    os.chmod(scratch, 0o700, follow_symlinks=False)
    scratch_identity = _identity(scratch)
    archive = scratch / release.archive.name
    manifest = scratch / release.manifest.name
    try:
        _download_official_asset(release.manifest, manifest, transport,
                                 MAX_MANIFEST_BYTES, timeout)
        _download_official_asset(release.archive, archive, transport,
                                 MAX_ARCHIVE_BYTES, timeout)
        yield release, archive, manifest
    finally:
        if (_matches_identity(scratch, scratch_identity) and not scratch.is_symlink()
                and scratch.is_dir()):
            shutil.rmtree(scratch)


@contextlib.contextmanager
def downloaded_official_support_release(
        version: str | None = None, *, transport: ReleaseTransport | None = None,
        timeout: float = EXPLICIT_RELEASE_TIMEOUT):
    transport = transport or ReleaseTransport()
    deadline = time.monotonic() + timeout
    release = discover_official_support_release(
        version, transport=transport, timeout=timeout, deadline=deadline,
    )
    try:
        scratch = Path(tempfile.mkdtemp(prefix="lingtai-desktop-support-download-"))
    except OSError as error:
        raise DesktopCLIError("support release temporary directory could not be created") from error
    os.chmod(scratch, 0o700, follow_symlinks=False)
    scratch_identity = _identity(scratch)
    manifest_path = scratch / SUPPORT_MANIFEST_NAME
    payload_paths = {name: scratch / name for name in SUPPORT_PAYLOAD_NAMES}
    identities: dict[str, tuple[int, int]] = {}
    try:
        _download_official_asset(
            release.manifest_asset, manifest_path, transport,
            MAX_SUPPORT_MANIFEST_BYTES, timeout, deadline,
        )
        manifest_raw, identities[SUPPORT_MANIFEST_NAME] = _read_managed_support_file(
            manifest_path, "downloaded support manifest", MAX_SUPPORT_MANIFEST_BYTES,
            expected_size=release.manifest_asset.size_bytes,
        )
        if manifest_raw != release.manifest_bytes:
            raise DesktopCLIError("downloaded support manifest changed after discovery")
        for declared in release.manifest.files:
            path = payload_paths[declared.name]
            _download_official_asset(
                release.payload_assets[declared.name], path, transport,
                MAX_SUPPORT_PAYLOAD_BYTES, timeout, deadline,
            )
            content, identities[declared.name] = _read_managed_support_file(
                path, f"downloaded support payload {declared.name}",
                MAX_SUPPORT_PAYLOAD_BYTES, expected_size=declared.size,
            )
            if _sha256_bytes(content) != declared.sha256:
                raise DesktopCLIError(
                    f"downloaded support payload {declared.name} SHA-256 does not match manifest"
                )
            try:
                compile(content, declared.name, "exec", dont_inherit=True)
            except (SyntaxError, ValueError, TypeError) as error:
                raise DesktopCLIError("downloaded support payload is not valid Python") from error
        if {entry.name for entry in os.scandir(scratch)} != {
                SUPPORT_MANIFEST_NAME, *SUPPORT_PAYLOAD_NAMES}:
            raise DesktopCLIError("downloaded support release file set changed")
        if not _matches_identity(scratch, scratch_identity) or any(
                not _matches_identity(scratch / name, identity)
                for name, identity in identities.items()):
            raise DesktopCLIError("downloaded support release changed during validation")
        yield release, manifest_path, payload_paths
    finally:
        if (_matches_identity(scratch, scratch_identity) and not scratch.is_symlink()
                and scratch.is_dir()):
            shutil.rmtree(scratch)


def _regular_nofollow(path: Path, label: str) -> os.stat_result:
    try:
        facts = path.lstat()
    except OSError as error:
        raise DesktopCLIError(f"{label} must be an existing regular file, not a symlink") from error
    if stat.S_ISLNK(facts.st_mode) or not stat.S_ISREG(facts.st_mode):
        raise DesktopCLIError(f"{label} must be an existing regular file, not a symlink")
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
        raise DesktopCLIError(
            f"{label} must be an existing regular file, not a symlink"
        ) from error
    if not stat.S_ISREG(facts.st_mode):
        os.close(descriptor)
        raise DesktopCLIError(f"{label} must be an existing regular file, not a symlink")
    return descriptor, facts


def _read_bytes_nofollow(path: Path, label: str, limit: int | None = None) -> bytes:
    try:
        descriptor, facts = _open_regular_nofollow(path, label)
        with os.fdopen(descriptor, "rb") as stream:
            if limit is not None and facts.st_size > limit:
                raise DesktopCLIError(f"{label} is too large")
            data = stream.read(-1 if limit is None else limit + 1)
    except DesktopCLIError:
        raise
    except OSError as error:
        raise DesktopCLIError(f"could not read {label}") from error
    if limit is not None and len(data) > limit:
        raise DesktopCLIError(f"{label} is too large")
    return data


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sha256_file(path: Path, label: str, maximum_bytes: int | None = None) -> str:
    digest = hashlib.sha256()
    try:
        descriptor, facts = _open_regular_nofollow(path, label)
        with os.fdopen(descriptor, "rb") as stream:
            if maximum_bytes is not None and facts.st_size > maximum_bytes:
                raise DesktopCLIError(f"{label} is too large")
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except DesktopCLIError:
        raise
    except OSError as error:
        raise DesktopCLIError(f"could not hash {label}") from error
    return digest.hexdigest()


def _canonical_json_bytes(value: object, maximum_bytes: int, label: str) -> bytes:
    try:
        payload = (json.dumps(
            value, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
        ) + "\n").encode("ascii")
    except (TypeError, ValueError, UnicodeError) as error:
        raise DesktopCLIError(f"{label} is not canonical JSON") from error
    if len(payload) > maximum_bytes:
        raise DesktopCLIError(f"{label} is too large")
    return payload


def _support_file_values(files: Sequence[SupportPayload]) -> list[dict[str, object]]:
    return [
        {"mode": item.mode, "name": item.name, "sha256": item.sha256, "size": item.size}
        for item in files
    ]


def _support_manifest_identity_value(
        support_version: str, release_tag: str, repository: str,
        bootstrap_protocol: int, minimum_bootstrap_protocol: int,
        files: Sequence[SupportPayload]) -> dict[str, object]:
    return {
        "bootstrap_protocol": bootstrap_protocol,
        "files": _support_file_values(files),
        "minimum_bootstrap_protocol": minimum_bootstrap_protocol,
        "release_tag": release_tag,
        "repository": repository,
        "schema": SUPPORT_MANIFEST_SCHEMA,
        "support_version": support_version,
    }


def _support_generation_id(identity_value: dict[str, object]) -> str:
    # Hash the canonical immutable manifest identity before adding its derived
    # generation_id field; hashing the full object would be circular. The full
    # canonical manifest receives its independent manifest_sha256 afterward.
    version = identity_value.get("support_version")
    if not _is_safe_version(version):
        raise DesktopCLIError("support version is not a safe x.y.z value")
    identity_bytes = _canonical_json_bytes(
        identity_value, MAX_SUPPORT_MANIFEST_BYTES, "support manifest identity",
    )
    return f"{version}-{_sha256_bytes(identity_bytes)[:SUPPORT_GENERATION_DIGEST_LENGTH]}"


def _is_safe_support_generation(value: object) -> bool:
    return (
        isinstance(value, str)
        and len(value) <= MAX_VERSION_LENGTH + 1 + SUPPORT_GENERATION_DIGEST_LENGTH
        and value.isascii()
        and SUPPORT_GENERATION_PATTERN.fullmatch(value) is not None
    )


def _support_generation_version(generation_id: str) -> str:
    if not _is_safe_support_generation(generation_id):
        raise DesktopCLIError("support generation identity is malformed")
    return generation_id.rsplit("-", 1)[0]


def build_support_manifest_bytes(
        support_version: str, release_tag: str,
        payloads: Mapping[str, bytes], *,
        bootstrap_protocol: int = SUPPORT_BOOTSTRAP_PROTOCOL,
        minimum_bootstrap_protocol: int = SUPPORT_BOOTSTRAP_PROTOCOL) -> bytes:
    if not _is_safe_version(support_version):
        raise DesktopCLIError("support version is not a safe x.y.z value")
    if release_tag != f"v{support_version}":
        raise DesktopCLIError("support release tag does not match support version")
    if set(payloads) != set(SUPPORT_PAYLOAD_NAMES):
        raise DesktopCLIError("support payload set is not exact")
    if (type(bootstrap_protocol) is not int
            or bootstrap_protocol != SUPPORT_BOOTSTRAP_PROTOCOL
            or type(minimum_bootstrap_protocol) is not int
            or minimum_bootstrap_protocol < 1
            or minimum_bootstrap_protocol > bootstrap_protocol):
        raise DesktopCLIError("support bootstrap protocol declaration is invalid")
    files: list[SupportPayload] = []
    for name in SUPPORT_PAYLOAD_NAMES:
        content = payloads[name]
        if not isinstance(content, bytes) or not 0 < len(content) <= MAX_SUPPORT_PAYLOAD_BYTES:
            raise DesktopCLIError(f"support payload {name} has an invalid size")
        files.append(SupportPayload(
            name, len(content), SUPPORT_PAYLOAD_MODE, _sha256_bytes(content),
        ))
    identity = _support_manifest_identity_value(
        support_version, release_tag, SUPPORT_REPOSITORY,
        bootstrap_protocol, minimum_bootstrap_protocol, files,
    )
    value = dict(identity)
    value["generation_id"] = _support_generation_id(identity)
    payload = _canonical_json_bytes(value, MAX_SUPPORT_MANIFEST_BYTES, "support manifest")
    # Keep construction and parser policy one source of truth.
    parse_support_manifest(payload)
    return payload


def parse_support_manifest(
        raw: bytes, *, installed_bootstrap_protocol: int = SUPPORT_BOOTSTRAP_PROTOCOL,
        require_canonical: bool = True) -> SupportManifest:
    if not isinstance(raw, bytes) or len(raw) > MAX_SUPPORT_MANIFEST_BYTES:
        raise DesktopCLIError("support manifest is too large")
    try:
        value = json.loads(raw, object_pairs_hook=_exact_json_object)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError, ValueError) as error:
        raise DesktopCLIError("support manifest is not bounded valid JSON") from error
    if not isinstance(value, dict) or set(value) != SUPPORT_MANIFEST_KEYS:
        raise DesktopCLIError("support manifest does not match the exact schema")
    if value.get("schema") != SUPPORT_MANIFEST_SCHEMA:
        raise DesktopCLIError("support manifest schema is invalid")
    support_version = value.get("support_version")
    release_tag = value.get("release_tag")
    repository = value.get("repository")
    bootstrap_protocol = value.get("bootstrap_protocol")
    minimum_protocol = value.get("minimum_bootstrap_protocol")
    if not _is_safe_version(support_version):
        raise DesktopCLIError("support version is not a safe x.y.z value")
    if release_tag != f"v{support_version}":
        raise DesktopCLIError("support release tag does not match support version")
    if repository != SUPPORT_REPOSITORY:
        raise DesktopCLIError("support manifest repository identity is invalid")
    if (type(installed_bootstrap_protocol) is not int
            or installed_bootstrap_protocol < 1):
        raise DesktopCLIError("installed bootstrap protocol is invalid")
    if (type(bootstrap_protocol) is not int or bootstrap_protocol < 1
            or type(minimum_protocol) is not int or minimum_protocol < 1
            or minimum_protocol > bootstrap_protocol):
        raise DesktopCLIError("support bootstrap protocol declaration is invalid")
    if (minimum_protocol > installed_bootstrap_protocol
            or bootstrap_protocol > SUPPORT_BOOTSTRAP_PROTOCOL):
        raise DesktopCLIError("support generation requires a newer stable bootstrap protocol")
    if bootstrap_protocol != SUPPORT_BOOTSTRAP_PROTOCOL:
        raise DesktopCLIError("support bootstrap protocol is unsupported")
    raw_files = value.get("files")
    if not isinstance(raw_files, list) or len(raw_files) != len(SUPPORT_PAYLOAD_NAMES):
        raise DesktopCLIError("support manifest payload set is not exact")
    files: list[SupportPayload] = []
    for expected_name, item in zip(SUPPORT_PAYLOAD_NAMES, raw_files):
        if not isinstance(item, dict) or set(item) != SUPPORT_FILE_KEYS:
            raise DesktopCLIError("support manifest payload entry does not match the exact schema")
        name, size, mode, digest = (
            item.get("name"), item.get("size"), item.get("mode"), item.get("sha256"),
        )
        if name != expected_name:
            raise DesktopCLIError("support manifest payload names or order are not exact")
        if (type(size) is not int or size <= 0 or size > MAX_SUPPORT_PAYLOAD_BYTES
                or type(mode) is not int or mode != SUPPORT_PAYLOAD_MODE
                or not isinstance(digest, str) or SHA_PATTERN.fullmatch(digest) is None):
            raise DesktopCLIError("support manifest payload facts are invalid")
        files.append(SupportPayload(name, size, mode, digest))
    generation_id = value.get("generation_id")
    identity = _support_manifest_identity_value(
        support_version, release_tag, repository,
        bootstrap_protocol, minimum_protocol, files,
    )
    if (not _is_safe_support_generation(generation_id)
            or generation_id != _support_generation_id(identity)):
        raise DesktopCLIError("support generation identity does not match canonical manifest content")
    canonical = dict(identity)
    canonical["generation_id"] = generation_id
    canonical_bytes = _canonical_json_bytes(
        canonical, MAX_SUPPORT_MANIFEST_BYTES, "support manifest",
    )
    if require_canonical and raw != canonical_bytes:
        raise DesktopCLIError("support manifest bytes are not canonical")
    return SupportManifest(
        support_version, generation_id, release_tag, repository,
        bootstrap_protocol, minimum_protocol, tuple(files), _sha256_bytes(canonical_bytes),
    )


def _read_managed_support_file(path: Path, label: str, maximum_bytes: int,
                               expected_size: int | None = None,
                               expected_mode: int = SUPPORT_PAYLOAD_MODE) -> tuple[bytes, tuple[int, int]]:
    descriptor: int | None = None
    try:
        descriptor, facts = _open_regular_nofollow(path, label)
        if (facts.st_uid != os.geteuid() or facts.st_nlink != 1
                or stat.S_IMODE(facts.st_mode) != expected_mode
                or facts.st_size <= 0 or facts.st_size > maximum_bytes
                or (expected_size is not None and facts.st_size != expected_size)):
            raise DesktopCLIError(f"{label} ownership, mode, or size is invalid")
        identity = (facts.st_dev, facts.st_ino)
        with os.fdopen(descriptor, "rb") as stream:
            descriptor = None
            content = stream.read(maximum_bytes + 1)
    except DesktopCLIError:
        raise
    except OSError as error:
        raise DesktopCLIError(f"could not read {label}") from error
    finally:
        if descriptor is not None:
            os.close(descriptor)
    if len(content) > maximum_bytes or (expected_size is not None and len(content) != expected_size):
        raise DesktopCLIError(f"{label} size changed while reading")
    return content, identity


def validate_support_generation(
        generation: Path, *,
        installed_bootstrap_protocol: int = SUPPORT_BOOTSTRAP_PROTOCOL) -> ValidatedSupportGeneration:
    generation = Path(generation)
    try:
        directory_facts = generation.lstat()
    except OSError as error:
        raise DesktopCLIError("support generation is missing") from error
    if (stat.S_ISLNK(directory_facts.st_mode) or not stat.S_ISDIR(directory_facts.st_mode)
            or stat.S_IMODE(directory_facts.st_mode) != SUPPORT_GENERATION_MODE
            or not _is_safe_support_generation(generation.name)):
        raise DesktopCLIError("support generation directory identity or mode is invalid")
    try:
        actual_names = {entry.name for entry in os.scandir(generation)}
    except OSError as error:
        raise DesktopCLIError("support generation could not be enumerated") from error
    expected_names = {SUPPORT_MANIFEST_NAME, *SUPPORT_PAYLOAD_NAMES}
    if actual_names != expected_names:
        raise DesktopCLIError("support generation file set is not exact")
    manifest_bytes, manifest_identity = _read_managed_support_file(
        generation / SUPPORT_MANIFEST_NAME, "support manifest",
        MAX_SUPPORT_MANIFEST_BYTES,
    )
    manifest = parse_support_manifest(
        manifest_bytes, installed_bootstrap_protocol=installed_bootstrap_protocol,
    )
    if manifest.generation_id != generation.name:
        raise DesktopCLIError("support generation directory does not match its manifest")
    identities: list[tuple[str, tuple[int, int]]] = [
        (SUPPORT_MANIFEST_NAME, manifest_identity),
    ]
    for payload in manifest.files:
        content, identity = _read_managed_support_file(
            generation / payload.name, f"support payload {payload.name}",
            MAX_SUPPORT_PAYLOAD_BYTES, payload.size, payload.mode,
        )
        if _sha256_bytes(content) != payload.sha256:
            raise DesktopCLIError(f"support payload {payload.name} SHA-256 does not match manifest")
        identities.append((payload.name, identity))
    return ValidatedSupportGeneration(
        generation, manifest, (directory_facts.st_dev, directory_facts.st_ino),
        tuple(identities),
    )


def _support_state_value(state: SupportState) -> dict[str, object]:
    return {
        "failed_generations": [
            {"generation_id": item.generation_id, "manifest_sha256": item.manifest_sha256}
            for item in state.failed_generations
        ],
        "high_water_manifest_sha256": state.high_water_manifest_sha256,
        "high_water_version": state.high_water_version,
        "last_good_generation": state.last_good_generation,
        "schema": SUPPORT_STATE_SCHEMA,
    }


def support_state_bytes(state: SupportState) -> bytes:
    payload = _canonical_json_bytes(
        _support_state_value(state), MAX_SUPPORT_STATE_BYTES, "support state",
    )
    parse_support_state(payload)
    return payload


def parse_support_state(raw: bytes, *, require_canonical: bool = True) -> SupportState:
    if not isinstance(raw, bytes) or len(raw) > MAX_SUPPORT_STATE_BYTES:
        raise DesktopCLIError("support state is too large")
    try:
        value = json.loads(raw, object_pairs_hook=_exact_json_object)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError, ValueError) as error:
        raise DesktopCLIError("support state is not bounded valid JSON") from error
    if (not isinstance(value, dict) or set(value) != SUPPORT_STATE_KEYS
            or value.get("schema") != SUPPORT_STATE_SCHEMA):
        raise DesktopCLIError("support state does not match the exact schema")
    version = value.get("high_water_version")
    digest = value.get("high_water_manifest_sha256")
    last_good = value.get("last_good_generation")
    if (not _is_safe_version(version)
            or not isinstance(digest, str) or SHA_PATTERN.fullmatch(digest) is None
            or not _is_safe_support_generation(last_good)):
        raise DesktopCLIError("support state high-water or last-good identity is invalid")
    raw_failed = value.get("failed_generations")
    if (not isinstance(raw_failed, list)
            or len(raw_failed) > MAX_FAILED_SUPPORT_GENERATIONS):
        raise DesktopCLIError("support state failed-generation list is invalid")
    failed: list[FailedSupportGeneration] = []
    seen: set[str] = set()
    for item in raw_failed:
        if not isinstance(item, dict) or set(item) != SUPPORT_FAILED_KEYS:
            raise DesktopCLIError("support failed-generation entry does not match the exact schema")
        generation_id, manifest_sha = item.get("generation_id"), item.get("manifest_sha256")
        if (not _is_safe_support_generation(generation_id)
                or not isinstance(manifest_sha, str)
                or SHA_PATTERN.fullmatch(manifest_sha) is None
                or generation_id in seen):
            raise DesktopCLIError("support failed-generation identity is invalid")
        seen.add(generation_id)
        failed.append(FailedSupportGeneration(generation_id, manifest_sha))
    state = SupportState(version, digest, last_good, tuple(failed))
    canonical = _canonical_json_bytes(
        _support_state_value(state), MAX_SUPPORT_STATE_BYTES, "support state",
    )
    if require_canonical and raw != canonical:
        raise DesktopCLIError("support state bytes are not canonical")
    return state


def _support_pending_value(pending: SupportPending) -> dict[str, object]:
    return {
        "expected_current_dev": pending.expected_current_dev,
        "expected_current_ino": pending.expected_current_ino,
        "explicit_retry": pending.explicit_retry,
        "from_generation": pending.from_generation,
        "requested_argv_sha256": pending.requested_argv_sha256,
        "rollback_pointer_name": pending.rollback_pointer_name,
        "schema": SUPPORT_PENDING_SCHEMA,
        "to_generation": pending.to_generation,
        "to_manifest_sha256": pending.to_manifest_sha256,
    }


def support_pending_bytes(pending: SupportPending) -> bytes:
    payload = _canonical_json_bytes(
        _support_pending_value(pending), MAX_SUPPORT_PENDING_BYTES, "support pending journal",
    )
    parse_support_pending(payload)
    return payload


def parse_support_pending(raw: bytes, *, require_canonical: bool = True) -> SupportPending:
    if not isinstance(raw, bytes) or len(raw) > MAX_SUPPORT_PENDING_BYTES:
        raise DesktopCLIError("support pending journal is too large")
    try:
        value = json.loads(raw, object_pairs_hook=_exact_json_object)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError, ValueError) as error:
        raise DesktopCLIError("support pending journal is not bounded valid JSON") from error
    if (not isinstance(value, dict) or set(value) != SUPPORT_PENDING_KEYS
            or value.get("schema") != SUPPORT_PENDING_SCHEMA):
        raise DesktopCLIError("support pending journal does not match the exact schema")
    from_generation = value.get("from_generation")
    to_generation = value.get("to_generation")
    digest = value.get("to_manifest_sha256")
    argv_digest = value.get("requested_argv_sha256")
    dev, ino = value.get("expected_current_dev"), value.get("expected_current_ino")
    explicit_retry = value.get("explicit_retry")
    rollback_pointer_name = value.get("rollback_pointer_name")
    if (not _is_safe_support_generation(from_generation)
            or not _is_safe_support_generation(to_generation)
            or from_generation == to_generation
            or not isinstance(digest, str) or SHA_PATTERN.fullmatch(digest) is None
            or not isinstance(argv_digest, str) or SHA_PATTERN.fullmatch(argv_digest) is None
            or type(dev) is not int or dev < 0 or type(ino) is not int or ino < 0
            or type(explicit_retry) is not bool
            or not isinstance(rollback_pointer_name, str)
            or SUPPORT_ROLLBACK_POINTER_PATTERN.fullmatch(rollback_pointer_name) is None):
        raise DesktopCLIError("support pending journal identities are invalid")
    pending = SupportPending(
        from_generation, to_generation, digest, dev, ino, argv_digest,
        explicit_retry, rollback_pointer_name,
    )
    canonical = _canonical_json_bytes(
        _support_pending_value(pending), MAX_SUPPORT_PENDING_BYTES,
        "support pending journal",
    )
    if require_canonical and raw != canonical:
        raise DesktopCLIError("support pending journal bytes are not canonical")
    return pending


def validate_support_candidate(
        candidate: SupportManifest, state: SupportState, *,
        explicit_retry: bool = False) -> None:
    candidate_version = _version_tuple(candidate.support_version)
    high_water_version = _version_tuple(state.high_water_version)
    if candidate_version < high_water_version:
        raise DesktopCLIError("support update refuses a version below the high-water mark")
    if (candidate_version == high_water_version
            and candidate.manifest_sha256 != state.high_water_manifest_sha256):
        raise DesktopCLIError("support update refuses same-version manifest substitution")
    if not explicit_retry and any(
            item.generation_id == candidate.generation_id
            and item.manifest_sha256 == candidate.manifest_sha256
            for item in state.failed_generations):
        raise DesktopCLIError("support update target was already recorded as failed")


def load_manifest(archive: Path, manifest: Path) -> Manifest:
    archive = Path(archive)
    manifest = Path(manifest)
    archive_descriptor, archive_stat = _open_regular_nofollow(archive, "App archive")
    os.close(archive_descriptor)
    if archive_stat.st_size > MAX_ARCHIVE_BYTES:
        raise DesktopCLIError("App archive is too large")
    raw = _read_bytes_nofollow(manifest, "manifest", 16 * 1024)
    try:
        data = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError) as error:
        raise DesktopCLIError("manifest must be bounded valid JSON") from error
    if not isinstance(data, dict) or set(data) != MANIFEST_KEYS:
        raise DesktopCLIError("manifest does not match the bounded exact schema")
    if type(data["schema_version"]) is not int or data["schema_version"] != MANIFEST_SCHEMA:
        raise DesktopCLIError("manifest schema version is invalid")
    version = data["version"]
    if not _is_safe_version(version):
        raise DesktopCLIError("manifest version is not a safe x.y.z value")
    expected_name = f"LingTai-{version}-macOS-universal.app.tar.gz"
    if data["archive_file_name"] != expected_name or archive.name != expected_name:
        raise DesktopCLIError("App archive does not have the exact deterministic file name")
    exact = {
        "artifact_kind": ARTIFACT_KIND,
        "schema_version": MANIFEST_SCHEMA,
        "bundle_name": APP_NAME,
        "bundle_identifier": BUNDLE_ID,
        "bundle_version": version,
        "bundle_executable": EXECUTABLE_RELATIVE,
        "minimum_macos": MINIMUM_MACOS,
    }
    if any(data[key] != value for key, value in exact.items()):
        raise DesktopCLIError("manifest artifact or App identity facts are invalid")
    if data["architectures"] != list(ARCHITECTURES):
        raise DesktopCLIError("manifest architectures must be arm64+x86_64")
    size_bytes = data["archive_size_bytes"]
    if type(size_bytes) is not int or size_bytes <= 0 or size_bytes != archive_stat.st_size:
        raise DesktopCLIError("App archive size does not match manifest")
    sha = data["archive_sha256"]
    if (not isinstance(sha, str) or SHA_PATTERN.fullmatch(sha) is None
            or _sha256_file(archive, "App archive", MAX_ARCHIVE_BYTES) != sha):
        raise DesktopCLIError("App archive SHA-256 does not match manifest")
    executable_size = data["executable_size_bytes"]
    executable_sha = data["executable_sha256"]
    bundle_digest = data["bundle_tree_sha256"]
    if type(executable_size) is not int or executable_size <= 0:
        raise DesktopCLIError("manifest executable size is invalid")
    if not isinstance(executable_sha, str) or SHA_PATTERN.fullmatch(executable_sha) is None:
        raise DesktopCLIError("manifest executable SHA-256 is invalid")
    if not isinstance(bundle_digest, str) or SHA_PATTERN.fullmatch(bundle_digest) is None:
        raise DesktopCLIError("manifest recursive bundle digest is invalid")
    git_head, git_tree, dirty = data["packaging_git_head"], data["packaging_git_tree"], data["packaging_git_dirty"]
    if not isinstance(git_head, str) or GIT_SHA_PATTERN.fullmatch(git_head) is None:
        raise DesktopCLIError("manifest packaging Git HEAD is invalid")
    if not isinstance(git_tree, str) or GIT_SHA_PATTERN.fullmatch(git_tree) is None or type(dirty) is not bool:
        raise DesktopCLIError("manifest packaging Git facts are invalid")
    return Manifest(
        version, expected_name, archive_stat.st_size, sha,
        executable_size, executable_sha, bundle_digest,
        git_head, git_tree, dirty, _sha256_bytes(raw),
    )


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
    support = root / "support"
    return ManagedPaths(
        raw, local, local / "bin", root, support, support / "versions",
        support / "current", support / "pending.json", support / "state.json",
        support / "update-check.json", root / "versions", root / "receipts",
        root / "current", root / "update-check.json", root / INITIAL_INSTALL_NAME,
        local / "bin/lingtai-desktop",
    )


def _require_nonroot(effective_uid: int | None) -> None:
    uid = os.geteuid() if effective_uid is None else effective_uid
    if uid == 0:
        raise DesktopCLIError("mutating commands refuse effective uid 0")


def _ensure_directory(
        path: Path, mode: int, *, preserve_existing_mode: bool = False,
        created_directories: list[tuple[Path, tuple[int, int]]] | None = None,
) -> None:
    created = False
    if path.exists() or path.is_symlink():
        facts = path.lstat()
        if stat.S_ISLNK(facts.st_mode) or not stat.S_ISDIR(facts.st_mode):
            raise DesktopCLIError(f"managed directory is not a real directory (symlink refused): {path.name}")
    else:
        path.mkdir(mode=mode)
        created = True
        facts = path.lstat()
        if created_directories is not None:
            created_directories.append((path, (facts.st_dev, facts.st_ino)))
    if created or not preserve_existing_mode:
        os.chmod(path, mode, follow_symlinks=False)


def _require_real_directory(path: Path, label: str) -> None:
    try:
        facts = path.lstat()
    except OSError as error:
        raise DesktopCLIError(f"{label} is missing") from error
    if stat.S_ISLNK(facts.st_mode) or not stat.S_ISDIR(facts.st_mode):
        raise DesktopCLIError(f"{label} is not a real directory (symlink refused)")


def _prepare_layout(
        paths: ManagedPaths,
        created_directories: list[tuple[Path, tuple[int, int]]] | None = None,
) -> None:
    _ensure_directory(
        paths.local, 0o700, preserve_existing_mode=True,
        created_directories=created_directories,
    )
    _ensure_directory(
        paths.bin, 0o700, preserve_existing_mode=True,
        created_directories=created_directories,
    )
    _ensure_directory(
        paths.local / "share", 0o700, preserve_existing_mode=True,
        created_directories=created_directories,
    )
    for directory in (
            paths.root, paths.support, paths.support_versions,
            paths.versions, paths.receipts):
        _ensure_directory(
            directory, 0o700, created_directories=created_directories,
        )


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
                kind, payload = "file", bytes.fromhex(_sha256_file(path, "bundle file"))
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
            digest.update(payload)
            digest.update(b"\0")
            if kind == "directory":
                visit(path)

    visit(app)
    return digest.hexdigest()


def _receipt(manifest: Manifest, bundle_digest: str) -> dict[str, object]:
    return {
        "schema_version": RECEIPT_SCHEMA, "artifact_kind": ARTIFACT_KIND,
        "version": manifest.version, "bundle_id": BUNDLE_ID,
        "bundle_version": manifest.version,
        "bundle_executable": EXECUTABLE_RELATIVE,
        "minimum_macos": MINIMUM_MACOS,
        "architectures": list(ARCHITECTURES),
        "source_archive": {"file_name": manifest.file_name, "size_bytes": manifest.size_bytes, "sha256": manifest.sha256},
        "manifest_sha256": manifest.manifest_sha256,
        "packaging_git_head": manifest.packaging_git_head,
        "packaging_git_tree": manifest.packaging_git_tree,
        "packaging_git_dirty": manifest.packaging_git_dirty,
        "executable_size_bytes": manifest.executable_size_bytes,
        "executable_sha256": manifest.executable_sha256,
        "bundle_tree_sha256": bundle_digest,
        "managed_app_path": f"versions/{manifest.version}/{APP_NAME}",
    }


def _launcher_bytes(repository_root: Path | None = None) -> bytes:
    roots = []
    if repository_root is not None:
        roots.extend((Path(repository_root) / "scripts", Path(repository_root)))
    roots.append(Path(__file__).parent)
    for root in roots:
        source = root / "support_bootstrap.py"
        if source.is_file() and not source.is_symlink():
            payload = _read_bytes_nofollow(
                source, "stable support bootstrap source", 256 * 1024,
            )
            if (_sha256_bytes(payload) != STABLE_BOOTSTRAP_SHA256
                    or LAUNCHER_MARKER.encode() not in payload[:512]):
                raise DesktopCLIError("stable support bootstrap source identity is invalid")
            return payload
    raise DesktopCLIError("stable support bootstrap source is missing")


def _identity(path: Path) -> tuple[int, int]:
    facts = path.lstat()
    return facts.st_dev, facts.st_ino


def _matches_identity(path: Path, expected: tuple[int, int]) -> bool:
    try:
        return _identity(path) == expected
    except OSError:
        return False


def _publish_file_exclusive(source: Path, destination: Path, mode: int) -> tuple[int, int]:
    identity: tuple[int, int] | None = None
    visible = False
    try:
        os.chmod(source, mode, follow_symlinks=False)
        facts = _regular_nofollow(source, f"staged {destination.name}")
        if stat.S_IMODE(facts.st_mode) != mode:
            raise DesktopCLIError(f"staged {destination.name} has an incorrect mode")
        identity = (facts.st_dev, facts.st_ino)
        os.link(source, destination, follow_symlinks=False)
        visible = True
        published = destination.lstat()
        if ((published.st_dev, published.st_ino) != identity
                or not stat.S_ISREG(published.st_mode)
                or stat.S_IMODE(published.st_mode) != mode):
            raise DesktopCLIError(f"{destination.name} publication was replaced")
        _fsync_directory(destination.parent)
        return identity
    except FileExistsError as error:
        raise DesktopCLIError(f"refusing to overwrite existing {destination.name}") from error
    except DesktopCLIError:
        if visible:
            _unlink_if_identity(destination, identity)
        raise
    except OSError as error:
        if visible:
            _unlink_if_identity(destination, identity)
        raise DesktopCLIError(f"could not publish {destination.name}") from error


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


def _exchange_paths(first: Path, second: Path) -> None:
    """Atomically exchange two same-filesystem names or fail closed."""
    libc = ctypes.CDLL(None, use_errno=True)
    if sys.platform == "darwin":
        exchange = libc.renamex_np
        exchange.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
        exchange.restype = ctypes.c_int
        result = exchange(os.fsencode(first), os.fsencode(second), 0x00000002)  # RENAME_SWAP
    else:
        exchange = getattr(libc, "renameat2", None)
        if exchange is None:
            raise DesktopCLIError("atomic current-pointer exchange is unavailable")
        exchange.argtypes = [
            ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p,
            ctypes.c_uint,
        ]
        exchange.restype = ctypes.c_int
        result = exchange(
            -100, os.fsencode(first), -100, os.fsencode(second),
            0x00000002,  # Linux RENAME_EXCHANGE
        )
    if result != 0:
        error_number = ctypes.get_errno()
        raise DesktopCLIError(
            f"could not atomically exchange managed current pointer ({error_number})"
        )


def _restore_app_current(
        paths: ManagedPaths, backup: Path, previous_identity: tuple[int, int],
        published_identity: tuple[int, int]) -> None:
    if (not _matches_identity(paths.current, published_identity)
            or not _matches_identity(backup, previous_identity)):
        raise DesktopCLIError(
            "refusing to restore changed App current pointer; new target is retained"
        )
    _exchange_paths(backup, paths.current)
    if (not _matches_identity(paths.current, previous_identity)
            or not _matches_identity(backup, published_identity)):
        raise DesktopCLIError(
            "App current rollback exchange did not restore exact previous pointer"
        )
    backup.unlink()
    _fsync_directory(paths.root)


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


def _validate_stable_launcher(paths: ManagedPaths) -> tuple[int, int]:
    payload, identity = _read_managed_support_file(
        paths.launcher, "stable support bootstrap", 256 * 1024,
        expected_mode=0o755,
    )
    if (_sha256_bytes(payload) != STABLE_BOOTSTRAP_SHA256
            or LAUNCHER_MARKER.encode() not in payload[:512]):
        raise DesktopCLIError("stable support bootstrap identity is invalid")
    return identity


def _support_current(paths: ManagedPaths) -> tuple[str, tuple[int, int]]:
    try:
        facts = paths.support_current.lstat()
    except OSError as error:
        raise DesktopCLIError("support current pointer is missing") from error
    if not stat.S_ISLNK(facts.st_mode) or facts.st_nlink != 1:
        raise DesktopCLIError("support current pointer is not a single-link symlink")
    target = os.readlink(paths.support_current)
    prefix = "versions/"
    generation = target[len(prefix):] if target.startswith(prefix) else ""
    if (not _is_safe_support_generation(generation)
            or target != f"versions/{generation}"):
        raise DesktopCLIError("support current pointer escapes or is malformed")
    return generation, (facts.st_dev, facts.st_ino)


def _read_support_state(paths: ManagedPaths) -> tuple[SupportState, tuple[int, int]]:
    payload, identity = _read_managed_support_file(
        paths.support_state, "support state", MAX_SUPPORT_STATE_BYTES,
    )
    return parse_support_state(payload), identity


def _read_support_pending(
        paths: ManagedPaths) -> tuple[SupportPending, tuple[int, int]] | None:
    if not paths.support_pending.exists() and not paths.support_pending.is_symlink():
        return None
    payload, identity = _read_managed_support_file(
        paths.support_pending, "support pending journal", MAX_SUPPORT_PENDING_BYTES,
    )
    return parse_support_pending(payload), identity


def _validate_support_state_relationship(
        paths: ManagedPaths, state: SupportState,
        known: Mapping[str, ValidatedSupportGeneration] | None = None,
) -> dict[str, ValidatedSupportGeneration]:
    generations = dict(known or {})

    def generation(name: str) -> ValidatedSupportGeneration:
        if name not in generations:
            generations[name] = validate_support_generation(
                paths.support_versions / name
            )
        return generations[name]

    last_good = generation(state.last_good_generation)
    last_good_version = _version_tuple(last_good.manifest.support_version)
    high_water_version = _version_tuple(state.high_water_version)
    if last_good_version > high_water_version:
        raise DesktopCLIError("support last-good version exceeds high-water")
    if (last_good_version == high_water_version
            and last_good.manifest.manifest_sha256
            != state.high_water_manifest_sha256):
        raise DesktopCLIError(
            "support high-water hash does not bind last-good generation"
        )
    failed_generations: list[ValidatedSupportGeneration] = []
    for failed in state.failed_generations:
        if failed.generation_id == state.last_good_generation:
            raise DesktopCLIError("support last-good generation cannot also be failed")
        candidate = generation(failed.generation_id)
        if candidate.manifest.manifest_sha256 != failed.manifest_sha256:
            raise DesktopCLIError(
                "support failed-generation hash does not bind its generation"
            )
        failed_generations.append(candidate)
    if last_good_version < high_water_version and not any(
            _version_tuple(item.manifest.support_version) == high_water_version
            and item.manifest.manifest_sha256 == state.high_water_manifest_sha256
            for item in failed_generations):
        raise DesktopCLIError(
            "support local rollback lacks an exact failed high-water generation"
        )
    return generations


def _validate_owned_cli(paths: ManagedPaths) -> ValidatedSupportGeneration:
    for directory, label in (
        (paths.support, "managed support"),
        (paths.support_versions, "managed support versions"),
    ):
        _require_real_directory(directory, label)
        if stat.S_IMODE(directory.stat().st_mode) != 0o700:
            raise DesktopCLIError(f"{label} mode is invalid")
    _validate_stable_launcher(paths)
    state, _ = _read_support_state(paths)
    current, _ = _support_current(paths)
    active = validate_support_generation(paths.support_versions / current)
    pending_loaded = _read_support_pending(paths)
    if pending_loaded is None:
        _validate_support_state_relationship(
            paths, state, {active.manifest.generation_id: active},
        )
        if state.last_good_generation != current:
            raise DesktopCLIError("support current and last-good state disagree")
        return active
    pending, _ = pending_loaded
    if current not in {pending.from_generation, pending.to_generation}:
        raise DesktopCLIError("support pending transaction does not match current pointer")
    source = validate_support_generation(paths.support_versions / pending.from_generation)
    target = validate_support_generation(paths.support_versions / pending.to_generation)
    if target.manifest.manifest_sha256 != pending.to_manifest_sha256:
        raise DesktopCLIError("support pending target manifest digest is invalid")
    _validate_support_state_relationship(paths, state, {
        source.manifest.generation_id: source,
        target.manifest.generation_id: target,
    })
    committed_recovery = (
        state.last_good_generation == target.manifest.generation_id
        and state.high_water_version == target.manifest.support_version
        and state.high_water_manifest_sha256 == target.manifest.manifest_sha256
    )
    if not committed_recovery and state.last_good_generation != source.manifest.generation_id:
        raise DesktopCLIError("support pending source is not last-good")
    return active


def _read_receipt(paths: ManagedPaths, version: str) -> dict[str, object]:
    if not _is_safe_version(version):
        raise DesktopCLIError("version must be a safe x.y.z value")
    receipt_path = paths.receipts / f"{version}.json"
    raw, _ = _read_managed_support_file(
        receipt_path, "receipt", 8192, expected_mode=0o600,
    )
    try:
        receipt = json.loads(raw, object_pairs_hook=_exact_json_object)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError, ValueError) as error:
        raise DesktopCLIError("receipt is invalid") from error
    if not isinstance(receipt, dict) or set(receipt) != RECEIPT_KEYS or receipt.get("schema_version") != RECEIPT_SCHEMA:
        raise DesktopCLIError("receipt does not match the bounded exact schema")
    expected_path = f"versions/{version}/{APP_NAME}"
    if receipt.get("version") != version or receipt.get("managed_app_path") != expected_path:
        raise DesktopCLIError("receipt version/path relation is invalid")
    if (receipt.get("artifact_kind") != ARTIFACT_KIND
            or receipt.get("bundle_id") != BUNDLE_ID
            or receipt.get("bundle_version") != version
            or receipt.get("bundle_executable") != EXECUTABLE_RELATIVE
            or receipt.get("minimum_macos") != MINIMUM_MACOS
            or receipt.get("architectures") != list(ARCHITECTURES)):
        raise DesktopCLIError("receipt App facts are invalid")
    source = receipt.get("source_archive")
    if not isinstance(source, dict) or set(source) != {"file_name", "size_bytes", "sha256"}:
        raise DesktopCLIError("receipt source artifact facts are invalid")
    if source.get("file_name") != f"LingTai-{version}-macOS-universal.app.tar.gz":
        raise DesktopCLIError("receipt source artifact name is invalid")
    if type(source.get("size_bytes")) is not int or source["size_bytes"] <= 0:
        raise DesktopCLIError("receipt source artifact size is invalid")
    if not isinstance(source.get("sha256"), str) or SHA_PATTERN.fullmatch(source["sha256"]) is None:
        raise DesktopCLIError("receipt source artifact SHA-256 is invalid")
    if not isinstance(receipt.get("manifest_sha256"), str) or SHA_PATTERN.fullmatch(receipt["manifest_sha256"]) is None:
        raise DesktopCLIError("receipt manifest SHA-256 is invalid")
    for field in ("packaging_git_head", "packaging_git_tree"):
        if not isinstance(receipt.get(field), str) or GIT_SHA_PATTERN.fullmatch(receipt[field]) is None:
            raise DesktopCLIError("receipt packaging-only Git facts are invalid")
    if type(receipt.get("packaging_git_dirty")) is not bool:
        raise DesktopCLIError("receipt packaging-only dirty fact is invalid")
    if type(receipt.get("executable_size_bytes")) is not int or receipt["executable_size_bytes"] <= 0:
        raise DesktopCLIError("receipt executable size is invalid")
    executable_sha = receipt.get("executable_sha256")
    if not isinstance(executable_sha, str) or SHA_PATTERN.fullmatch(executable_sha) is None:
        raise DesktopCLIError("receipt executable SHA-256 is invalid")
    digest = receipt.get("bundle_tree_sha256")
    if not isinstance(digest, str) or SHA_PATTERN.fullmatch(digest) is None:
        raise DesktopCLIError("receipt bundle digest is invalid")
    canonical = (json.dumps(receipt, indent=2, sort_keys=True) + "\n").encode()
    if raw != canonical:
        raise DesktopCLIError("receipt bytes are not canonical")
    return receipt


def _active(paths: ManagedPaths) -> tuple[str, Path, dict[str, object]]:
    if not paths.current.is_symlink():
        if paths.current.exists():
            raise DesktopCLIError("managed current is not a symlink")
        raise DesktopCLIError("no current LingTai Desktop version is installed")
    for directory, label in ((paths.local, "managed .local"), (paths.bin, "managed bin"),
                             (paths.root, "managed root"),
                             (paths.versions, "managed versions"), (paths.receipts, "managed receipts")):
        _require_real_directory(directory, label)
    target = os.readlink(paths.current)
    prefix = "versions/"
    version = target[len(prefix):] if target.startswith(prefix) else ""
    if not _is_safe_version(version):
        raise DesktopCLIError("managed current symlink escapes or is malformed")
    version_directory = paths.versions / version
    _require_real_directory(version_directory, "managed version")
    app = version_directory / APP_NAME
    receipt = _read_receipt(paths, version)
    executable = _inspect_app(app, version)
    executable_facts = _regular_nofollow(executable, "App executable")
    if (executable_facts.st_size != receipt["executable_size_bytes"]
            or _sha256_file(executable, "App executable") != receipt["executable_sha256"]):
        raise DesktopCLIError("installed executable facts do not match receipt")
    if bundle_tree_digest(app) != receipt["bundle_tree_sha256"]:
        raise DesktopCLIError("installed bundle digest does not match receipt")
    return version, app, receipt


def _preflight_uninstall(paths: ManagedPaths) -> UninstallPlan:
    """Preflight only the App plane; support bytes never gate --version removal."""
    for directory, label in (
        (paths.local, "managed .local"), (paths.bin, "managed bin"),
        (paths.local / "share", "managed share"), (paths.root, "managed root"),
        (paths.versions, "managed versions"), (paths.receipts, "managed receipts"),
    ):
        _require_real_directory(directory, label)

    current_target: str | None = None
    current_identity: tuple[int, int] | None = None
    if paths.current.is_symlink():
        current_target = os.readlink(paths.current)
        current_identity = _identity(paths.current)
        current_version = current_target.removeprefix("versions/")
        if (not current_target.startswith("versions/")
                or current_target != f"versions/{current_version}"
                or not _is_safe_version(current_version)):
            raise DesktopCLIError("managed current symlink escapes or is malformed")
    elif paths.current.exists():
        raise DesktopCLIError("managed current is not a symlink")

    allowed_root = {"versions", "receipts"}
    if current_target is not None:
        allowed_root.add("current")
    update_cache_identity: tuple[int, int] | None = None
    if paths.update_cache.exists() or paths.update_cache.is_symlink():
        _read_update_cache(paths)
        update_cache_identity = _identity(paths.update_cache)
        allowed_root.add(paths.update_cache.name)
    actual_root = {path.name for path in paths.root.iterdir()}
    # support is foreign to this App-only proof: presence, absence, type, and
    # contents must neither authorize nor gate --version.
    actual_root.discard("support")
    if actual_root != allowed_root:
        raise DesktopCLIError("managed App root contains unknown or missing files")

    receipt_by_version: dict[str, Path] = {}
    for receipt_path in sorted(paths.receipts.iterdir(), key=lambda path: path.name):
        version = receipt_path.name.removesuffix(".json")
        facts = receipt_path.lstat()
        if (not receipt_path.name.endswith(".json") or not _is_safe_version(version)
                or stat.S_ISLNK(facts.st_mode) or not stat.S_ISREG(facts.st_mode)):
            raise DesktopCLIError("receipts contains unknown files")
        receipt_by_version[version] = receipt_path
    version_by_name: dict[str, Path] = {}
    for version_path in sorted(paths.versions.iterdir(), key=lambda path: path.name):
        if not _is_safe_version(version_path.name):
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
        executable = _inspect_app(app, version)
        executable_facts = _regular_nofollow(executable, "App executable")
        if (executable_facts.st_size != receipt["executable_size_bytes"]
                or _sha256_file(executable, "App executable") != receipt["executable_sha256"]):
            raise DesktopCLIError("installed executable facts do not match receipt")
        if bundle_tree_digest(app) != receipt["bundle_tree_sha256"]:
            raise DesktopCLIError("installed bundle digest does not match receipt")
        entries.append(UninstallEntry(
            version, receipt, _identity(version_path), _identity(receipt_path)
        ))
    return UninstallPlan(
        tuple(entries), _identity(paths.root), _identity(paths.versions),
        _identity(paths.receipts), update_cache_identity,
        current_target, current_identity,
    )


def _support_update_cache_value(check: SupportUpdateCheck) -> dict[str, object]:
    return {
        "checked_at": check.checked_at,
        "declined": check.declined,
        "generation_id": check.generation_id,
        "latest_support_version": check.latest_support_version,
        "manifest_sha256": check.manifest_sha256,
        "release_tag": check.release_tag,
        "schema": SUPPORT_UPDATE_CACHE_SCHEMA,
    }


def _support_update_cache_bytes(check: SupportUpdateCheck) -> bytes:
    if (type(check.checked_at) is not int or check.checked_at < 0
            or not _is_safe_version(check.latest_support_version)
            or check.release_tag != f"v{check.latest_support_version}"
            or not _is_safe_support_generation(check.generation_id)
            or _support_generation_version(check.generation_id)
            != check.latest_support_version
            or not isinstance(check.manifest_sha256, str)
            or SHA_PATTERN.fullmatch(check.manifest_sha256) is None
            or type(check.declined) is not bool):
        raise DesktopCLIError("support update-check values are invalid")
    return _canonical_json_bytes(
        _support_update_cache_value(check), MAX_SUPPORT_UPDATE_CACHE_BYTES,
        "support update-check cache",
    )


def _read_support_update_cache(paths: ManagedPaths) -> SupportUpdateCheck | None:
    path = paths.support_update_cache
    if not path.exists() and not path.is_symlink():
        return None
    raw, _ = _read_managed_support_file(
        path, "support update-check cache", MAX_SUPPORT_UPDATE_CACHE_BYTES,
    )
    try:
        value = json.loads(raw, object_pairs_hook=_exact_json_object)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError, ValueError) as error:
        raise DesktopCLIError("support update-check cache is invalid") from error
    if (not isinstance(value, dict) or set(value) != SUPPORT_UPDATE_CACHE_KEYS
            or value.get("schema") != SUPPORT_UPDATE_CACHE_SCHEMA):
        raise DesktopCLIError("support update-check cache does not match the exact schema")
    check = SupportUpdateCheck(
        value.get("checked_at"), value.get("latest_support_version"),
        value.get("release_tag"), value.get("generation_id"),
        value.get("manifest_sha256"), value.get("declined"),
    )
    canonical = _support_update_cache_bytes(check)
    if raw != canonical:
        raise DesktopCLIError("support update-check cache bytes are not canonical")
    return check


def _write_support_update_cache(paths: ManagedPaths, check: SupportUpdateCheck) -> None:
    """Atomically exchange an existing cache and restore its exact inode on failure."""
    _require_real_directory(paths.support, "managed support")
    payload = _support_update_cache_bytes(check)
    path = paths.support_update_cache
    previous_identity: tuple[int, int] | None = None
    if path.exists() or path.is_symlink():
        _read_support_update_cache(paths)
        previous_identity = _identity(path)
    temporary = paths.support / f".update-check-{uuid.uuid4().hex}"
    temporary_identity: tuple[int, int] | None = None
    published = False
    retained_previous = False
    try:
        temporary_identity = _write_private_staged(
            temporary, payload, "support update-check cache",
        )
        if previous_identity is None:
            try:
                os.link(temporary, path, follow_symlinks=False)
            except FileExistsError as error:
                raise DesktopCLIError("refusing to replace a raced support update-check cache") from error
            temporary.unlink()
            published = True
        else:
            if not _matches_identity(path, previous_identity):
                raise DesktopCLIError("refusing to replace a raced support update-check cache")
            _exchange_paths(temporary, path)
            published = True
            if (not _matches_identity(path, temporary_identity)
                    or not _matches_identity(temporary, previous_identity)):
                # If the destination changed in the final pre-exchange window,
                # swap it back rather than overwriting or deleting the racer.
                if (_matches_identity(path, temporary_identity)
                        and (temporary.exists() or temporary.is_symlink())):
                    _exchange_paths(temporary, path)
                    published = False
                raise DesktopCLIError("refusing to replace a raced support update-check cache")
            retained_previous = True
        raw, identity = _read_managed_support_file(
            path, "support update-check cache", MAX_SUPPORT_UPDATE_CACHE_BYTES,
            expected_size=len(payload),
        )
        if raw != payload or identity != temporary_identity:
            raise DesktopCLIError("support update-check cache publication was replaced")
        _fsync_directory(paths.support)
        if retained_previous:
            if not _matches_identity(temporary, previous_identity):
                raise DesktopCLIError("prior support update-check cache was replaced")
            temporary.unlink()
            retained_previous = False
            _fsync_directory(paths.support)
    except Exception:
        if (previous_identity is not None and published
                and temporary_identity is not None
                and _matches_identity(path, temporary_identity)
                and _matches_identity(temporary, previous_identity)):
            _exchange_paths(temporary, path)
            published = False
            retained_previous = False
            _fsync_directory(paths.support)
        elif (previous_identity is None and published
              and temporary_identity is not None
              and _matches_identity(path, temporary_identity)):
            path.unlink()
            published = False
            _fsync_directory(paths.support)
        raise
    finally:
        # After a successful restoration the staged inode is back at the
        # temporary name. Never unlink a retained prior/racer identity here.
        _unlink_if_identity(temporary, temporary_identity)


def _preflight_support_uninstall(paths: ManagedPaths) -> SupportUninstallPlan:
    _validate_owned_cli(paths)
    state, state_identity = _read_support_state(paths)
    current, current_identity = _support_current(paths)
    pending_loaded = _read_support_pending(paths)
    allowed = {"versions", "current", "state.json"}
    pending_identity: tuple[int, int] | None = None
    rollback_pointer_name: str | None = None
    rollback_pointer_identity: tuple[int, int] | None = None
    if pending_loaded is not None:
        pending, pending_identity = pending_loaded
        allowed.add("pending.json")
        rollback = paths.support / pending.rollback_pointer_name
        if rollback.exists() or rollback.is_symlink():
            expected = (pending.expected_current_dev, pending.expected_current_ino)
            try:
                facts = rollback.lstat()
                target = os.readlink(rollback)
            except OSError as error:
                raise DesktopCLIError("support rollback backup is invalid") from error
            if (not stat.S_ISLNK(facts.st_mode)
                    or (facts.st_dev, facts.st_ino) != expected
                    or target != f"versions/{pending.from_generation}"):
                raise DesktopCLIError("support rollback backup is invalid")
            rollback_pointer_name = pending.rollback_pointer_name
            rollback_pointer_identity = expected
            allowed.add(pending.rollback_pointer_name)
    cache = _read_support_update_cache(paths)
    cache_identity = (_identity(paths.support_update_cache)
                      if cache is not None else None)
    if cache is not None:
        allowed.add("update-check.json")
    if {path.name for path in paths.support.iterdir()} != allowed:
        raise DesktopCLIError("managed support contains unknown or missing files")
    generations: list[ValidatedSupportGeneration] = []
    for path in sorted(paths.support_versions.iterdir(), key=lambda item: item.name):
        if not _is_safe_support_generation(path.name):
            raise DesktopCLIError("support versions contains unknown files")
        generations.append(validate_support_generation(path))
    names = {item.manifest.generation_id for item in generations}
    referenced = {current, state.last_good_generation}
    referenced.update(item.generation_id for item in state.failed_generations)
    if pending_loaded is not None:
        pending, _ = pending_loaded
        referenced.update((pending.from_generation, pending.to_generation))
    if not referenced.issubset(names):
        raise DesktopCLIError("support state references a missing generation")
    return SupportUninstallPlan(
        _identity(paths.support), _identity(paths.support_versions),
        tuple(generations), f"versions/{current}", current_identity,
        state_identity, pending_identity,
        rollback_pointer_name, rollback_pointer_identity, cache_identity,
        _validate_stable_launcher(paths),
    )


def _revalidate_uninstall_ancestors(paths: ManagedPaths, plan: UninstallPlan) -> None:
    for directory, identity, label in (
        (paths.root, plan.root_identity, "managed root"),
        (paths.versions, plan.versions_identity, "managed versions"),
        (paths.receipts, plan.receipts_identity, "managed receipts"),
    ):
        _require_real_directory(directory, label)
        if not _matches_identity(directory, identity):
            raise DesktopCLIError(f"refusing mutation through replaced {label}")


def _revalidate_support_uninstall(
        paths: ManagedPaths, plan: SupportUninstallPlan) -> None:
    for directory, identity, label in (
        (paths.support, plan.support_identity, "managed support"),
        (paths.support_versions, plan.versions_identity, "managed support versions"),
    ):
        _require_real_directory(directory, label)
        if not _matches_identity(directory, identity):
            raise DesktopCLIError(f"refusing mutation through replaced {label}")


def _version_tuple(version: str) -> tuple[int, int, int]:
    if not _is_safe_version(version):
        raise DesktopCLIError("version must be a safe x.y.z value")
    return tuple(int(value) for value in version.split("."))  # type: ignore[return-value]


def _read_update_cache(paths: ManagedPaths) -> UpdateCheck | None:
    path = paths.update_cache
    if not path.exists() and not path.is_symlink():
        return None
    facts = _regular_nofollow(path, "managed update-check cache")
    if stat.S_IMODE(facts.st_mode) != 0o600 or facts.st_nlink != 1:
        raise DesktopCLIError("managed update-check cache ownership or mode is invalid")
    raw = _read_bytes_nofollow(path, "managed update-check cache", MAX_UPDATE_CACHE_BYTES)
    try:
        value = json.loads(raw, object_pairs_hook=_exact_json_object)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError, ValueError) as error:
        raise DesktopCLIError("managed update-check cache is invalid") from error
    if (not isinstance(value, dict) or set(value) != UPDATE_CACHE_KEYS
            or type(value.get("schema_version")) is not int
            or value.get("schema_version") != UPDATE_CACHE_SCHEMA
            or type(value.get("checked_at")) is not int
            or value["checked_at"] < 0
            or not _is_safe_version(value.get("latest_version"))):
        raise DesktopCLIError("managed update-check cache does not match the bounded exact schema")
    return UpdateCheck(value["checked_at"], value["latest_version"])


def _update_cache_bytes(latest_version: str, checked_at: int) -> bytes:
    _version_tuple(latest_version)
    if type(checked_at) is not int or checked_at < 0:
        raise DesktopCLIError("update-check time is invalid")
    payload = (json.dumps({
        "checked_at": checked_at,
        "latest_version": latest_version,
        "schema_version": UPDATE_CACHE_SCHEMA,
    }, sort_keys=True, separators=(",", ":")) + "\n").encode()
    if len(payload) > MAX_UPDATE_CACHE_BYTES:
        raise DesktopCLIError("update-check cache payload is too large")
    return payload


def _write_update_cache(paths: ManagedPaths, latest_version: str,
                        checked_at: int) -> None:
    _require_real_directory(paths.root, "managed root")
    previous_identity: tuple[int, int] | None = None
    if paths.update_cache.exists() or paths.update_cache.is_symlink():
        _read_update_cache(paths)
        previous_identity = _identity(paths.update_cache)
    payload = _update_cache_bytes(latest_version, checked_at)
    temporary = paths.root / f".update-check-{uuid.uuid4().hex}"
    temporary_identity: tuple[int, int] | None = None
    publication_valid = False
    descriptor: int | None = None
    try:
        flags = (os.O_WRONLY | os.O_CREAT | os.O_EXCL
                 | getattr(os, "O_NOFOLLOW", 0))
        descriptor = os.open(temporary, flags, 0o600)
        with os.fdopen(descriptor, "wb") as stream:
            descriptor = None
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o600, follow_symlinks=False)
        facts = _regular_nofollow(temporary, "staged update-check cache")
        temporary_identity = (facts.st_dev, facts.st_ino)
        if (facts.st_size != len(payload) or stat.S_IMODE(facts.st_mode) != 0o600
                or facts.st_nlink != 1):
            raise DesktopCLIError("staged update-check cache facts are invalid")
        if previous_identity is None:
            try:
                os.link(temporary, paths.update_cache, follow_symlinks=False)
            except FileExistsError as error:
                raise DesktopCLIError("refusing to replace a raced update-check cache") from error
            temporary.unlink()
        else:
            if not _matches_identity(paths.update_cache, previous_identity):
                raise DesktopCLIError("refusing to replace a raced update-check cache")
            os.replace(temporary, paths.update_cache)
        published = _regular_nofollow(paths.update_cache, "managed update-check cache")
        if ((published.st_dev, published.st_ino) != temporary_identity
                or published.st_size != len(payload)
                or stat.S_IMODE(published.st_mode) != 0o600
                or published.st_nlink != 1):
            raise DesktopCLIError("update-check cache publication was replaced")
        publication_valid = True
    except DesktopCLIError:
        raise
    except OSError as error:
        raise DesktopCLIError("could not publish managed update-check cache") from error
    finally:
        if descriptor is not None:
            os.close(descriptor)
        _unlink_if_identity(temporary, temporary_identity)
        if not publication_valid:
            _unlink_if_identity(paths.update_cache, temporary_identity)


def _fsync_directory(path: Path) -> None:
    descriptor: int | None = None
    try:
        descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        os.fsync(descriptor)
    except OSError as error:
        raise DesktopCLIError(f"could not durably publish {path.name}") from error
    finally:
        if descriptor is not None:
            os.close(descriptor)


def _write_private_staged(path: Path, payload: bytes, label: str) -> tuple[int, int]:
    descriptor: int | None = None
    try:
        descriptor = os.open(
            path, os.O_WRONLY | os.O_CREAT | os.O_EXCL
            | getattr(os, "O_NOFOLLOW", 0), 0o600,
        )
        with os.fdopen(descriptor, "wb") as stream:
            descriptor = None
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(path, 0o600, follow_symlinks=False)
        content, identity = _read_managed_support_file(
            path, f"staged {label}", max(len(payload), 1),
            expected_size=len(payload),
        )
        if content != payload:
            raise DesktopCLIError(f"staged {label} bytes changed")
        return identity
    except DesktopCLIError:
        raise
    except OSError as error:
        raise DesktopCLIError(f"could not stage {label}") from error
    finally:
        if descriptor is not None:
            os.close(descriptor)


def _publish_private_file_exclusive(path: Path, payload: bytes, label: str) -> tuple[int, int]:
    temporary = path.parent / f".{path.name}-{uuid.uuid4().hex}"
    temporary_identity: tuple[int, int] | None = None
    visible = False
    try:
        temporary_identity = _write_private_staged(temporary, payload, label)
        try:
            os.link(temporary, path, follow_symlinks=False)
        except FileExistsError as error:
            raise DesktopCLIError(f"refusing to replace existing {label}") from error
        visible = True
        temporary.unlink()
        published, identity = _read_managed_support_file(
            path, label, max(len(payload), 1), expected_size=len(payload),
        )
        if published != payload or identity != temporary_identity:
            raise DesktopCLIError(f"{label} publication was replaced")
        _fsync_directory(path.parent)
        return identity
    except Exception:
        if visible:
            _unlink_if_identity(path, temporary_identity)
        raise
    finally:
        _unlink_if_identity(temporary, temporary_identity)


def _verified_support_payloads(
        manifest_bytes: bytes, payloads: Mapping[str, bytes]) -> SupportManifest:
    manifest = parse_support_manifest(manifest_bytes)
    if set(payloads) != set(SUPPORT_PAYLOAD_NAMES):
        raise DesktopCLIError("support payload set is not exact")
    for declared in manifest.files:
        content = payloads.get(declared.name)
        if (not isinstance(content, bytes) or len(content) != declared.size
                or _sha256_bytes(content) != declared.sha256):
            raise DesktopCLIError(
                f"support payload {declared.name} does not match authoritative manifest"
            )
        try:
            compile(content, declared.name, "exec", dont_inherit=True)
        except (SyntaxError, ValueError, TypeError) as error:
            raise DesktopCLIError("support payload is not valid Python") from error
    rebuilt = build_support_manifest_bytes(
        manifest.support_version, manifest.release_tag, payloads,
    )
    if rebuilt != manifest_bytes:
        raise DesktopCLIError("support manifest is not the canonical payload authority")
    return manifest


def _publish_support_generation(
        paths: ManagedPaths, payloads: Mapping[str, bytes], *,
        support_version: str | None = None, release_tag: str | None = None,
        manifest_bytes: bytes | None = None) -> ValidatedSupportGeneration:
    if manifest_bytes is None:
        if support_version is None or release_tag is None:
            raise DesktopCLIError("support generation publication lacks manifest identity")
        manifest_bytes = build_support_manifest_bytes(
            support_version, release_tag, payloads,
        )
    manifest = _verified_support_payloads(manifest_bytes, payloads)
    if (support_version is not None and manifest.support_version != support_version
            or release_tag is not None and manifest.release_tag != release_tag):
        raise DesktopCLIError("support generation arguments disagree with manifest")
    generation_id = manifest.generation_id
    final = paths.support_versions / generation_id
    if final.exists() or final.is_symlink():
        existing = validate_support_generation(final)
        if existing.manifest.manifest_sha256 != _sha256_bytes(manifest_bytes):
            raise DesktopCLIError("support generation collision has different bytes")
        return existing
    scratch = Path(tempfile.mkdtemp(prefix=".support-generation-", dir=paths.support))
    scratch_identity = _identity(scratch)
    os.chmod(scratch, SUPPORT_GENERATION_MODE, follow_symlinks=False)
    published_identity: tuple[int, int] | None = None
    staged_files: dict[str, tuple[int, int]] = {}
    try:
        staged_files[SUPPORT_MANIFEST_NAME] = _write_private_staged(
            scratch / SUPPORT_MANIFEST_NAME, manifest_bytes, "support manifest",
        )
        for name in SUPPORT_PAYLOAD_NAMES:
            staged_files[name] = _write_private_staged(
                scratch / name, payloads[name], f"support payload {name}",
            )
        _fsync_directory(scratch)
        staged_identity = _identity(scratch)
        _rename_exclusive(scratch, final)
        published_identity = staged_identity
        _fsync_directory(paths.support_versions)
        if _FAILPOINT == "support-generation-published":
            raise DesktopCLIError("injected support generation publication failure")
        validated = validate_support_generation(final)
        if validated.directory_identity != published_identity:
            raise DesktopCLIError("support generation publication was replaced")
        return validated
    except Exception:
        if (published_identity is not None
                and _matches_identity(final, published_identity)):
            try:
                actual = {entry.name for entry in os.scandir(final)}
                exact = actual == set(staged_files) and all(
                    _matches_identity(final / name, identity)
                    for name, identity in staged_files.items()
                )
            except OSError:
                exact = False
            if exact:
                shutil.rmtree(final)
                _fsync_directory(paths.support_versions)
        raise
    finally:
        if (_matches_identity(scratch, scratch_identity)
                and not scratch.is_symlink() and scratch.is_dir()):
            shutil.rmtree(scratch)


def _remove_validated_support_generation(
        generation: ValidatedSupportGeneration) -> None:
    if not _matches_identity(generation.path, generation.directory_identity):
        raise DesktopCLIError("refusing to remove a replaced support generation")
    current = validate_support_generation(generation.path)
    if (current.directory_identity != generation.directory_identity
            or current.file_identities != generation.file_identities):
        raise DesktopCLIError("refusing to remove a mutated support generation")
    shutil.rmtree(generation.path)


def _initial_support_state(generation: ValidatedSupportGeneration) -> SupportState:
    return SupportState(
        generation.manifest.support_version,
        generation.manifest.manifest_sha256,
        generation.manifest.generation_id,
        (),
    )


def _publish_initial_support(
        paths: ManagedPaths, module_bytes: bytes, verifier_bytes: bytes,
        launcher_bytes: bytes, *, support_version: str,
        release_tag: str, scratch: Path) -> InitialSupportPublication:
    payloads = {
        SUPPORT_PAYLOAD_NAMES[0]: module_bytes,
        SUPPORT_PAYLOAD_NAMES[1]: verifier_bytes,
    }
    manifest_bytes = build_support_manifest_bytes(
        support_version, release_tag, payloads,
    )
    generation_id = json.loads(manifest_bytes)["generation_id"]
    generation_path = paths.support_versions / generation_id
    generation_created = not (
        generation_path.exists() or generation_path.is_symlink()
    )
    state_created = False
    current_created = False
    launcher_created = False
    generation: ValidatedSupportGeneration | None = None
    state_identity: tuple[int, int] | None = None
    current_identity: tuple[int, int] | None = None
    launcher_identity: tuple[int, int] | None = None
    try:
        generation = _publish_support_generation(
            paths, payloads, support_version=support_version,
            release_tag=release_tag,
        )
        _trip_initial("support-generation")
        expected_state = support_state_bytes(_initial_support_state(generation))
        if paths.support_state.exists() or paths.support_state.is_symlink():
            existing_state, state_identity = _read_managed_support_file(
                paths.support_state, "support state", MAX_SUPPORT_STATE_BYTES,
                expected_size=len(expected_state),
            )
            if existing_state != expected_state:
                raise DesktopCLIError("initial support state does not match recovery journal")
        else:
            state_identity = _publish_private_file_exclusive(
                paths.support_state, expected_state, "support state",
            )
            state_created = True
        if _FAILPOINT == "support-state-published":
            raise DesktopCLIError("injected support state publication failure")
        _trip_initial("support-state")

        expected_target = f"versions/{generation.manifest.generation_id}"
        if paths.support_current.exists() or paths.support_current.is_symlink():
            current_name, current_identity = _support_current(paths)
            if current_name != generation.manifest.generation_id:
                raise DesktopCLIError(
                    "initial support current does not match recovery journal"
                )
        else:
            try:
                os.symlink(expected_target, paths.support_current)
            except FileExistsError as error:
                raise DesktopCLIError(
                    "refusing to replace existing support current pointer"
                ) from error
            except OSError as error:
                raise DesktopCLIError(
                    "could not publish initial support current pointer"
                ) from error
            current_identity = _identity(paths.support_current)
            current_created = True
            _fsync_directory(paths.support)
        if os.readlink(paths.support_current) != expected_target:
            raise DesktopCLIError("initial support current target is invalid")
        if _FAILPOINT == "support-current-published":
            raise DesktopCLIError("injected support current publication failure")
        _trip_initial("support-current")

        if paths.launcher.exists() or paths.launcher.is_symlink():
            installed, launcher_identity = _read_managed_support_file(
                paths.launcher, "stable support bootstrap", 256 * 1024,
                expected_size=len(launcher_bytes), expected_mode=0o755,
            )
            if installed != launcher_bytes:
                raise DesktopCLIError(
                    "stable support bootstrap does not match recovery journal"
                )
        else:
            staged_launcher = scratch / "lingtai-desktop"
            staged_launcher.write_bytes(launcher_bytes)
            try:
                launcher_identity = _publish_file_exclusive(
                    staged_launcher, paths.launcher, 0o755,
                )
                launcher_created = True
            finally:
                _unlink_if_identity(staged_launcher, _identity(staged_launcher)
                                    if staged_launcher.exists() else None)
        if _FAILPOINT == "launcher-post-visible":
            raise DesktopCLIError("injected launcher post-publication failure")
        _trip_initial("support-launcher")
        _validate_owned_cli(paths)
        if _FAILPOINT == "support-validation":
            raise DesktopCLIError("injected initial support validation failure")
        _trip_initial("support-validation")
        assert generation is not None
        assert state_identity is not None
        assert current_identity is not None
        assert launcher_identity is not None
        return InitialSupportPublication(
            generation, state_identity, current_identity, launcher_identity,
            generation_created, state_created, current_created, launcher_created,
        )
    except Exception:
        if launcher_created:
            _unlink_if_identity(paths.launcher, launcher_identity)
        if current_created:
            _unlink_if_identity(paths.support_current, current_identity)
        if state_created:
            _unlink_if_identity(paths.support_state, state_identity)
        if generation_created and generation is not None:
            _remove_validated_support_generation(generation)
        raise


def _argv_sha256(argv: Sequence[str]) -> str:
    try:
        value = [os.fspath(item) for item in argv]
        payload = _canonical_json_bytes(value, 64 * 1024, "support re-exec argv identity")
    except (TypeError, ValueError, UnicodeError) as error:
        raise DesktopCLIError("support re-exec argv is invalid") from error
    return _sha256_bytes(payload)


def _stage_verified_support_payloads(
        manifest_bytes: bytes, payloads: Mapping[str, bytes], *,
        argv: Sequence[str] | None, environment: Mapping[str, str] | None,
        home: Path | None, effective_uid: int | None, explicit_retry: bool,
        exec_launcher: Callable[[Path, list[str], dict[str, str]], object] | None,
        allow_no_change: bool) -> str | None:
    _require_nonroot(effective_uid)
    candidate = _verified_support_payloads(manifest_bytes, payloads)
    paths = _paths(home)
    current_generation = _validate_owned_cli(paths)
    if _read_support_pending(paths) is not None:
        raise DesktopCLIError("a support update transaction is already pending")
    state, _ = _read_support_state(paths)
    current_name, current_identity = _support_current(paths)
    if current_name != current_generation.manifest.generation_id:
        raise DesktopCLIError("support current changed during staging")
    requested_argv = list(sys.argv if argv is None else argv)
    if not requested_argv:
        raise DesktopCLIError("support re-exec argv must include the launcher")
    live_argv = [os.fspath(paths.launcher), *requested_argv[1:]]
    _argv_sha256(live_argv)
    if candidate.generation_id == current_name:
        if allow_no_change:
            return None
        raise DesktopCLIError("local support fixture is already active")
    validate_support_candidate(candidate, state, explicit_retry=explicit_retry)
    confirmed_name, confirmed_identity = _support_current(paths)
    if confirmed_name != current_name or confirmed_identity != current_identity:
        raise DesktopCLIError("support current changed during candidate validation")

    final = paths.support_versions / candidate.generation_id
    generation_preexisting = final.exists() or final.is_symlink()
    target = _publish_support_generation(
        paths, payloads, manifest_bytes=manifest_bytes,
    )
    if (target.manifest != candidate
            or target.manifest.manifest_sha256 != _sha256_bytes(manifest_bytes)):
        if not generation_preexisting:
            _remove_validated_support_generation(target)
        raise DesktopCLIError("published support generation differs from validated candidate")
    pending = SupportPending(
        current_name, target.manifest.generation_id,
        target.manifest.manifest_sha256,
        current_identity[0], current_identity[1], _argv_sha256(live_argv),
        explicit_retry, f".rollback-{uuid.uuid4().hex}",
    )
    try:
        _publish_private_file_exclusive(
            paths.support_pending, support_pending_bytes(pending),
            "support pending journal",
        )
    except Exception:
        if not generation_preexisting:
            _remove_validated_support_generation(target)
        raise
    if _FAILPOINT == "support-pending-published":
        raise DesktopCLIError("injected support pending publication failure")
    live_environment = dict(os.environ if environment is None else environment)
    live_environment[SUPPORT_REEXEC_MARKER] = "1"
    executor = exec_launcher or (
        lambda launcher, arguments, env: os.execve(launcher, arguments, env)
    )
    executor(paths.launcher, live_argv, live_environment)
    return target.manifest.generation_id


def stage_local_support_update(
        module_path: Path, verifier_path: Path, *,
        support_version: str, release_tag: str,
        argv: Sequence[str] | None = None,
        environment: Mapping[str, str] | None = None,
        home: Path | None = None,
        effective_uid: int | None = None,
        explicit_retry: bool = False,
        exec_launcher: Callable[[Path, list[str], dict[str, str]], object] | None = None) -> str:
    """Stage an offline local generation fixture; no network or cache is touched."""
    module_bytes, _ = _read_managed_support_file(
        Path(module_path), "local support CLI fixture", MAX_SUPPORT_PAYLOAD_BYTES,
    )
    verifier_bytes, _ = _read_managed_support_file(
        Path(verifier_path), "local support verifier fixture", MAX_SUPPORT_PAYLOAD_BYTES,
    )
    payloads = {
        SUPPORT_PAYLOAD_NAMES[0]: module_bytes,
        SUPPORT_PAYLOAD_NAMES[1]: verifier_bytes,
    }
    manifest_bytes = build_support_manifest_bytes(
        support_version, release_tag, payloads,
    )
    result = _stage_verified_support_payloads(
        manifest_bytes, payloads, argv=argv, environment=environment,
        home=home, effective_uid=effective_uid, explicit_retry=explicit_retry,
        exec_launcher=exec_launcher, allow_no_change=False,
    )
    assert result is not None
    return result


def stage_official_support_update(
        version: str | None = None, *, argv: Sequence[str] | None = None,
        environment: Mapping[str, str] | None = None,
        home: Path | None = None, transport: ReleaseTransport | None = None,
        timeout: float = EXPLICIT_RELEASE_TIMEOUT,
        effective_uid: int | None = None, explicit_retry: bool = False,
        exec_launcher: Callable[[Path, list[str], dict[str, str]], object] | None = None) -> str | None:
    """Fully download, validate, then stage one official support transaction."""
    with downloaded_official_support_release(
            version, transport=transport, timeout=timeout) as downloaded:
        release, manifest_path, payload_paths = downloaded
        manifest_bytes, _ = _read_managed_support_file(
            manifest_path, "verified official support manifest",
            MAX_SUPPORT_MANIFEST_BYTES,
            expected_size=release.manifest_asset.size_bytes,
        )
        payloads: dict[str, bytes] = {}
        for declared in release.manifest.files:
            payloads[declared.name], _ = _read_managed_support_file(
                payload_paths[declared.name],
                f"verified official support payload {declared.name}",
                MAX_SUPPORT_PAYLOAD_BYTES, expected_size=declared.size,
            )
        return _stage_verified_support_payloads(
            manifest_bytes, payloads, argv=argv, environment=environment,
            home=home, effective_uid=effective_uid,
            explicit_retry=explicit_retry, exec_launcher=exec_launcher,
            allow_no_change=True,
        )


def support_self_test() -> bool:
    """Bounded local/no-write contract invoked by the stable bootstrap in a child."""
    if SUPPORT_BOOTSTRAP_PROTOCOL != 1:
        return False
    module = Path(__file__)
    generation = module.parent
    validated = validate_support_generation(generation)
    if validated.path != generation or module.name != SUPPORT_PAYLOAD_NAMES[0]:
        return False
    if not any(
            name == SUPPORT_PAYLOAD_NAMES[0] and identity == _identity(module)
            for name, identity in validated.file_identities):
        return False
    installed_parser()
    bootstrap_parser()
    return True


def _initial_install_value(journal: InitialInstallJournal) -> dict[str, object]:
    return {
        "app_manifest_sha256": journal.app_manifest_sha256,
        "app_version": journal.app_version,
        "nonce": journal.nonce,
        "schema": INITIAL_INSTALL_SCHEMA,
        "support_generation": journal.support_generation,
        "support_manifest_sha256": journal.support_manifest_sha256,
    }


def _initial_install_bytes(journal: InitialInstallJournal) -> bytes:
    return _canonical_json_bytes(
        _initial_install_value(journal), MAX_INITIAL_INSTALL_BYTES,
        "initial-install journal",
    )


def _parse_initial_install(raw: bytes) -> InitialInstallJournal:
    if not isinstance(raw, bytes) or len(raw) > MAX_INITIAL_INSTALL_BYTES:
        raise DesktopCLIError("initial-install journal is too large")
    try:
        value = json.loads(raw, object_pairs_hook=_exact_json_object)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError, ValueError) as error:
        raise DesktopCLIError("initial-install journal is not bounded valid JSON") from error
    if (not isinstance(value, dict) or set(value) != INITIAL_INSTALL_KEYS
            or value.get("schema") != INITIAL_INSTALL_SCHEMA):
        raise DesktopCLIError("initial-install journal does not match exact schema")
    nonce = value.get("nonce")
    version = value.get("app_version")
    app_digest = value.get("app_manifest_sha256")
    generation = value.get("support_generation")
    support_digest = value.get("support_manifest_sha256")
    if (not isinstance(nonce, str) or re.fullmatch(r"[0-9a-f]{32}", nonce) is None
            or not _is_safe_version(version)
            or not isinstance(app_digest, str) or SHA_PATTERN.fullmatch(app_digest) is None
            or not _is_safe_support_generation(generation)
            or not isinstance(support_digest, str)
            or SHA_PATTERN.fullmatch(support_digest) is None):
        raise DesktopCLIError("initial-install journal identities are invalid")
    journal = InitialInstallJournal(
        nonce, version, app_digest, generation, support_digest,
    )
    if raw != _initial_install_bytes(journal):
        raise DesktopCLIError("initial-install journal bytes are not canonical")
    return journal


def _read_initial_install(
        paths: ManagedPaths,
) -> tuple[InitialInstallJournal, tuple[int, int]] | None:
    if not paths.initial_install.exists() and not paths.initial_install.is_symlink():
        return None
    raw, identity = _read_managed_support_file(
        paths.initial_install, "initial-install journal", MAX_INITIAL_INSTALL_BYTES,
    )
    return _parse_initial_install(raw), identity


def _remove_created_directories(
        created: Sequence[tuple[Path, tuple[int, int]]],
) -> None:
    for directory, identity in reversed(tuple(created)):
        if not _matches_identity(directory, identity):
            continue
        try:
            directory.rmdir()
        except OSError:
            # A non-empty directory or raced replacement is outside this cleanup
            # authority and must be preserved.
            continue


def _empty_initial_skeleton(paths: ManagedPaths) -> bool:
    try:
        if paths.launcher.exists() or paths.launcher.is_symlink():
            return False
        if not paths.root.is_dir() or paths.root.is_symlink():
            return False
        if {item.name for item in paths.root.iterdir()} != {
                "support", "versions", "receipts"}:
            return False
        if {item.name for item in paths.support.iterdir()} != {"versions"}:
            return False
        return all(
            directory.is_dir() and not directory.is_symlink()
            and stat.S_IMODE(directory.stat().st_mode) == 0o700
            and not any(directory.iterdir())
            for directory in (
                paths.support_versions, paths.versions, paths.receipts,
            )
        )
    except OSError:
        return False


def install(archive: Path, manifest_path: Path, *, home: Path | None = None,
            platform: Platform | None = None,
            effective_uid: int | None = None, update: bool = False,
            repository_root: Path | None = None) -> str:
    _require_nonroot(effective_uid)
    paths = _paths(home)
    manifest = load_manifest(Path(archive), Path(manifest_path))
    platform = platform or Platform()
    module_path = Path(__file__).resolve()
    repository_root = repository_root or (
        module_path.parent
        if (module_path.parent / "verify-app-archive.py").is_file()
        else module_path.parents[1]
    )
    if paths.root.exists() or paths.root.is_symlink():
        if paths.root.is_symlink() or not paths.root.is_dir():
            raise DesktopCLIError("managed root is a symlink or not a directory")

    loaded_journal = _read_initial_install(paths) if paths.root.exists() else None
    resume_initial = loaded_journal is not None
    skeleton_resume = (
        not resume_initial and paths.support.exists()
        and _empty_initial_skeleton(paths)
    )
    support_preexisting = paths.support.exists() or paths.support.is_symlink()
    needs_initial_support = (
        not support_preexisting or resume_initial or skeleton_resume
    )
    module_bytes = b""
    verifier_bytes = b""
    launcher_bytes = b""
    expected_journal: InitialInstallJournal | None = None
    journal_identity: tuple[int, int] | None = None
    if needs_initial_support:
        verifier_source = (
            repository_root / "scripts/verify-app-archive.py"
            if (repository_root / "scripts/verify-app-archive.py").is_file()
            else repository_root / "verify-app-archive.py"
        )
        module_bytes = Path(__file__).read_bytes()
        verifier_bytes = _read_bytes_nofollow(
            verifier_source, "independent package verifier",
            MAX_SUPPORT_PAYLOAD_BYTES,
        )
        launcher_bytes = _launcher_bytes(repository_root)
        support_manifest_bytes = build_support_manifest_bytes(
            manifest.version, f"v{manifest.version}", {
                SUPPORT_PAYLOAD_NAMES[0]: module_bytes,
                SUPPORT_PAYLOAD_NAMES[1]: verifier_bytes,
            },
        )
        support_manifest_value = json.loads(support_manifest_bytes)
        expected_journal = InitialInstallJournal(
            uuid.uuid4().hex, manifest.version, manifest.manifest_sha256,
            support_manifest_value["generation_id"],
            _sha256_bytes(support_manifest_bytes),
        )
        if loaded_journal is not None:
            journal, journal_identity = loaded_journal
            if dataclasses.replace(expected_journal, nonce=journal.nonce) != journal:
                raise DesktopCLIError(
                    "initial-install journal does not authorize these exact artifacts"
                )
            expected_journal = journal
    if support_preexisting and not resume_initial and not skeleton_resume:
        _validate_owned_cli(paths)

    current_version: str | None = None
    current_receipt: dict[str, object] | None = None
    if not resume_initial and not skeleton_resume and (
            paths.current.exists() or paths.current.is_symlink()):
        current_version, _, current_receipt = _active(paths)
    if update:
        if resume_initial or skeleton_resume or current_version is None:
            raise DesktopCLIError("update requires an installed current version")
        _validate_owned_cli(paths)
        if _version_tuple(manifest.version) < _version_tuple(current_version):
            raise DesktopCLIError("update refuses lower versions")
        if manifest.version == current_version:
            assert current_receipt is not None
            if current_receipt != _receipt(manifest, manifest.bundle_tree_sha256):
                raise DesktopCLIError(
                    "same-version update requires an identical artifact and receipt"
                )
            platform.verify_archive(repository_root, Path(archive), Path(manifest_path))
            return manifest.version
    elif current_version is not None:
        raise DesktopCLIError("an installation already exists; use update")

    # Archive and all support inputs are parsed/verified before the first mutation.
    platform.verify_archive(repository_root, Path(archive), Path(manifest_path))
    created_directories: list[tuple[Path, tuple[int, int]]] = []
    receipt_identity: tuple[int, int] | None = None
    version_identity: tuple[int, int] | None = None
    receipt_created = False
    version_created = False
    app_current_identity: tuple[int, int] | None = None
    app_current_created = False
    app_previous_current_identity: tuple[int, int] | None = None
    app_current_backup: Path | None = None
    support_publication: InitialSupportPublication | None = None
    journal_created = False
    managed_root_identity: tuple[int, int] | None = None
    scratch: Path | None = None
    scratch_identity: tuple[int, int] | None = None
    digest = ""
    try:
        _prepare_layout(paths, created_directories)
        managed_root_identity = _identity(paths.root)
        if not support_preexisting or resume_initial or skeleton_resume:
            if loaded_journal is None:
                assert expected_journal is not None
                journal_identity = _publish_private_file_exclusive(
                    paths.initial_install, _initial_install_bytes(expected_journal),
                    "initial-install journal",
                )
                journal_created = True
            _trip_initial("journal")

        final_version = paths.versions / manifest.version
        final_receipt = paths.receipts / f"{manifest.version}.json"
        if (not resume_initial and not skeleton_resume
                and (final_version.exists() or final_version.is_symlink()
                     or final_receipt.exists() or final_receipt.is_symlink())):
            raise DesktopCLIError("version collision: managed version already exists")

        scratch = Path(tempfile.mkdtemp(prefix="lingtai-desktop-initial-"))
        scratch.chmod(0o700)
        scratch_identity = _identity(scratch)
        staged_version = scratch / "version"
        staged_app = platform.verify_and_extract_archive(
            repository_root, Path(archive), Path(manifest_path), staged_version
        )
        executable = _inspect_app(staged_app, manifest.version)
        executable_facts = _regular_nofollow(executable, "App executable")
        if (executable_facts.st_size != manifest.executable_size_bytes
                or _sha256_file(executable, "App executable")
                != manifest.executable_sha256):
            raise DesktopCLIError("extracted executable facts do not match manifest")
        fake_home, fake_tmp = scratch / "smoke-home", scratch / "smoke-tmp"
        fake_home.mkdir(mode=0o700)
        fake_tmp.mkdir(mode=0o700)
        platform.smoke(executable, fake_home, fake_tmp)
        digest = bundle_tree_digest(staged_app)
        if digest != manifest.bundle_tree_sha256:
            raise DesktopCLIError(
                "extracted recursive bundle digest does not match manifest"
            )
        if _FAILPOINT == "extraction":
            raise DesktopCLIError("injected extraction validation failure")
        _trip_initial("extraction")

        receipt_bytes = (
            json.dumps(_receipt(manifest, digest), indent=2, sort_keys=True) + "\n"
        ).encode()
        staged_receipt = scratch / "receipt.json"
        staged_receipt.write_bytes(receipt_bytes)
        staged_receipt.chmod(0o600)
        if _FAILPOINT == "receipt":
            raise DesktopCLIError("injected receipt publication failure")
        if final_receipt.exists() or final_receipt.is_symlink():
            if not (resume_initial or skeleton_resume):
                raise DesktopCLIError("version collision: managed receipt exists")
            installed_receipt, receipt_identity = _read_managed_support_file(
                final_receipt, "initial App receipt", max(len(receipt_bytes), 1),
                expected_size=len(receipt_bytes),
            )
            if installed_receipt != receipt_bytes:
                raise DesktopCLIError(
                    "initial App receipt does not match recovery journal"
                )
        else:
            receipt_identity = _publish_file_exclusive(
                staged_receipt, final_receipt, 0o600,
            )
            staged_receipt.unlink()
            receipt_created = True
        _trip_initial("receipt")

        if final_version.exists() or final_version.is_symlink():
            if not (resume_initial or skeleton_resume):
                raise DesktopCLIError("version collision: managed version exists")
            _require_real_directory(final_version, "initial managed version")
            if ({item.name for item in final_version.iterdir()} != {APP_NAME}
                    or bundle_tree_digest(final_version / APP_NAME) != digest):
                raise DesktopCLIError(
                    "initial managed version does not match recovery journal"
                )
            version_identity = _identity(final_version)
        else:
            staged_version_identity = _identity(staged_version)
            _rename_exclusive(staged_version, final_version)
            version_identity = staged_version_identity
            version_created = True
            _fsync_directory(paths.versions)
        _trip_initial("version")

        if not support_preexisting or resume_initial or skeleton_resume:
            if _owned_file_state(paths.launcher, launcher_bytes, 0o755, "launcher"):
                if not resume_initial:
                    raise DesktopCLIError(
                        "refusing incomplete pre-existing support bootstrap"
                    )
            if _FAILPOINT == "launcher":
                raise DesktopCLIError("injected launcher publication failure")
            support_publication = _publish_initial_support(
                paths, module_bytes, verifier_bytes, launcher_bytes,
                support_version=manifest.version,
                release_tag=f"v{manifest.version}", scratch=scratch,
            )
        _trip_initial("support")

        old_target: str | None = None
        observed_current_identity: tuple[int, int] | None = None
        if paths.current.is_symlink():
            current_facts = paths.current.lstat()
            if current_facts.st_nlink != 1:
                raise DesktopCLIError("managed current is not a single-link symlink")
            old_target = os.readlink(paths.current)
            observed_current_identity = (current_facts.st_dev, current_facts.st_ino)
            if not _matches_identity(paths.current, observed_current_identity):
                raise DesktopCLIError("managed current changed during observation")
        elif paths.current.exists():
            raise DesktopCLIError("managed current is not a symlink")
        expected_current = f"versions/{manifest.version}"
        if resume_initial and old_target is not None:
            if old_target != expected_current:
                raise DesktopCLIError(
                    "initial App current does not match recovery journal"
                )
            app_current_identity = observed_current_identity
        else:
            temporary_current = paths.root / f".current-{uuid.uuid4().hex}"
            temporary_identity: tuple[int, int] | None = None
            try:
                os.symlink(expected_current, temporary_current)
                temporary_identity = _identity(temporary_current)
                if _FAILPOINT == "current":
                    raise DesktopCLIError("injected current switch failure")
                if old_target is None:
                    try:
                        os.link(
                            temporary_current, paths.current,
                            follow_symlinks=False,
                        )
                    except FileExistsError as error:
                        raise DesktopCLIError(
                            "refusing raced App current publication"
                        ) from error
                    if not _matches_identity(paths.current, temporary_identity):
                        raise DesktopCLIError("managed current publication was replaced")
                    temporary_current.unlink()
                    app_current_identity = temporary_identity
                    app_current_created = True
                else:
                    assert observed_current_identity is not None
                    if not _matches_identity(paths.current, observed_current_identity):
                        raise DesktopCLIError(
                            "refusing to replace raced App current pointer"
                        )
                    _exchange_paths(temporary_current, paths.current)
                    app_current_identity = temporary_identity
                    app_previous_current_identity = observed_current_identity
                    app_current_backup = temporary_current
                    if (not _matches_identity(paths.current, temporary_identity)
                            or not _matches_identity(
                                temporary_current, observed_current_identity,
                            )
                            or os.readlink(temporary_current) != old_target):
                        # The exchange captured a concurrent replacement. Put that
                        # exact inode back before rejecting this transaction.
                        raced_identity = _identity(temporary_current)
                        if (not _matches_identity(paths.current, temporary_identity)
                                or not _matches_identity(
                                    temporary_current, raced_identity,
                                )):
                            raise DesktopCLIError(
                                "App current changed during race reconciliation; "
                                "new target is retained"
                            )
                        _exchange_paths(temporary_current, paths.current)
                        if not _matches_identity(paths.current, raced_identity):
                            raise DesktopCLIError(
                                "raced App current pointer could not be restored"
                            )
                        app_current_identity = None
                        app_previous_current_identity = None
                        app_current_backup = None
                        raise DesktopCLIError(
                            "refusing to overwrite raced App current pointer"
                        )
                _fsync_directory(paths.root)
            finally:
                _unlink_if_identity(temporary_current, temporary_identity)
        if os.readlink(paths.current) != expected_current:
            raise DesktopCLIError("current switch did not take effect")
        if _FAILPOINT == "app-current-post-visible":
            raise DesktopCLIError("injected App current post-publication failure")
        _trip_initial("app-current")

        # A journal is removed only after both independent planes fully validate.
        _active(paths)
        _validate_owned_cli(paths)
        if journal_identity is not None:
            if not _matches_identity(paths.initial_install, journal_identity):
                raise DesktopCLIError("initial-install journal was replaced")
            paths.initial_install.unlink()
            _fsync_directory(paths.root)
        _trip_initial("journal-removed")
        if app_current_backup is not None:
            assert app_current_identity is not None
            assert app_previous_current_identity is not None
            if (not _matches_identity(paths.current, app_current_identity)
                    or not _matches_identity(
                        app_current_backup, app_previous_current_identity,
                    )):
                raise DesktopCLIError(
                    "App current changed before transaction commit; "
                    "rollback target is retained"
                )
            app_current_backup.unlink()
            app_current_backup = None
            _fsync_directory(paths.root)
        return manifest.version
    except InjectedInitialInstallCrash:
        raise
    except BaseException:
        # Restore or remove App current before deleting any version or receipt it
        # may still select. If exact restoration cannot be authenticated, stop
        # cleanup and retain the new target rather than strand the pointer.
        if (managed_root_identity is not None
                and _matches_identity(paths.root, managed_root_identity)):
            if app_current_backup is not None:
                assert app_current_identity is not None
                assert app_previous_current_identity is not None
                _restore_app_current(
                    paths, app_current_backup, app_previous_current_identity,
                    app_current_identity,
                )
                app_current_backup = None
            elif app_current_created and app_current_identity is not None:
                _unlink_if_identity(paths.current, app_current_identity)
                _fsync_directory(paths.root)
        if support_publication is not None:
            if support_publication.launcher_created:
                _unlink_if_identity(
                    paths.launcher, support_publication.launcher_identity,
                )
            if managed_root_identity is not None and _matches_identity(
                    paths.root, managed_root_identity):
                if support_publication.current_created:
                    _unlink_if_identity(
                        paths.support_current, support_publication.current_identity,
                    )
                if support_publication.state_created:
                    _unlink_if_identity(
                        paths.support_state, support_publication.state_identity,
                    )
                if support_publication.generation_created:
                    _remove_validated_support_generation(
                        support_publication.generation
                    )
        if (version_created and version_identity is not None
                and _matches_identity(paths.versions / manifest.version, version_identity)):
            _remove_owned_version(
                paths.versions / manifest.version, digest, version_identity,
            )
        if receipt_created:
            _unlink_if_identity(
                paths.receipts / f"{manifest.version}.json", receipt_identity,
            )
        if journal_created:
            _unlink_if_identity(paths.initial_install, journal_identity)
        _remove_created_directories(created_directories)
        raise
    finally:
        if (scratch is not None and scratch_identity is not None
                and _matches_identity(scratch, scratch_identity)
                and not scratch.is_symlink() and scratch.is_dir()):
            shutil.rmtree(scratch)


def install_official(version: str | None = None, *, home: Path | None = None,
                     platform: Platform | None = None,
                     transport: ReleaseTransport | None = None,
                     effective_uid: int | None = None, update: bool = False,
                     repository_root: Path | None = None) -> str:
    with downloaded_official_release(version, transport=transport) as release_pair:
        release, archive, manifest = release_pair
        installed = install(
            archive, manifest, home=home, platform=platform,
            effective_uid=effective_uid, update=update,
            repository_root=repository_root,
        )
        if installed != release.version:
            raise DesktopCLIError("installed version does not match official release metadata")
        return installed


def _default_tty() -> bool:
    return sys.stdin.isatty() and sys.stdout.isatty()


def _record_support_update_cache(
        paths: ManagedPaths, check: SupportUpdateCheck,
        output: Callable[[str], None]) -> None:
    try:
        _write_support_update_cache(paths, check)
    except DesktopCLIError as error:
        output(f"Support update cache was not recorded: {error}")


def _automatic_support_update_offer(
        *, paths: ManagedPaths, arguments: Sequence[str], home: Path | None,
        transport: ReleaseTransport | None, effective_uid: int | None,
        now: int, interval: int, tty: Callable[[], bool],
        prompt: Callable[[str], str], output: Callable[[str], None],
        exec_launcher: Callable[[Path, list[str], dict[str, str]], object] | None) -> bool:
    """Fail-open support metadata cadence; return true only after re-exec was requested."""
    try:
        cached = _read_support_update_cache(paths)
    except DesktopCLIError as error:
        output(f"Support update cache is invalid; continuing without a support check: {error}")
        return False
    fresh = cached is not None and 0 <= now - cached.checked_at < interval
    if fresh:
        check = cached
        assert check is not None
    else:
        try:
            deadline = time.monotonic() + AUTOMATIC_RELEASE_TIMEOUT
            release = discover_official_support_release(
                transport=transport, timeout=AUTOMATIC_RELEASE_TIMEOUT,
                deadline=deadline,
            )
            check = SupportUpdateCheck(
                now, release.version, release.manifest.release_tag,
                release.manifest.generation_id,
                release.manifest.manifest_sha256, False,
            )
        except DesktopCLIError as error:
            output(f"Support update check failed; continuing with installed support: {error}")
            return False

    try:
        active = _validate_owned_cli(paths)
        state, _ = _read_support_state(paths)
        current_version = active.manifest.support_version
        if check.generation_id == active.manifest.generation_id:
            if not fresh:
                _record_support_update_cache(paths, check, output)
            return False
        if _version_tuple(check.latest_support_version) <= _version_tuple(current_version):
            if not fresh:
                _record_support_update_cache(paths, check, output)
            return False
        # Fresh cache entries may decide whether discovery is due, but never
        # override rollback/substitution/failed-target policy.
        if _version_tuple(check.latest_support_version) < _version_tuple(state.high_water_version):
            raise DesktopCLIError("cached support target is below the high-water mark")
        if (_version_tuple(check.latest_support_version)
                == _version_tuple(state.high_water_version)
                and check.manifest_sha256 != state.high_water_manifest_sha256):
            raise DesktopCLIError("cached support target substitutes a same-version manifest")
        if any(
                failed.generation_id == check.generation_id
                and failed.manifest_sha256 == check.manifest_sha256
                for failed in state.failed_generations):
            raise DesktopCLIError("support target was already recorded as failed")
    except DesktopCLIError as error:
        output(f"Support update check was rejected; continuing with installed support: {error}")
        return False

    if fresh and check.declined:
        return False
    notice = (
        f"Support update available: {check.latest_support_version} "
        f"(installed {current_version}); run 'lingtai-desktop update'."
    )
    try:
        interactive = bool(tty())
    except Exception:
        interactive = False
    if not interactive:
        _record_support_update_cache(paths, dataclasses.replace(check, declined=False), output)
        output(notice)
        return False
    try:
        answer = prompt(
            f"LingTai Desktop support {check.latest_support_version} is available "
            f"(installed {current_version}). Stage it now? [y/N] "
        )
    except (EOFError, OSError):
        answer = ""
    if answer.strip().lower() not in {"y", "yes"}:
        _record_support_update_cache(paths, dataclasses.replace(check, declined=True), output)
        return False
    try:
        staged = stage_official_support_update(
            check.latest_support_version,
            argv=[os.fspath(paths.launcher), *list(arguments)],
            home=home, transport=transport, effective_uid=effective_uid,
            exec_launcher=exec_launcher,
        )
    except DesktopCLIError as error:
        output(f"Support update failed; continuing with installed support: {error}")
        return False
    _record_support_update_cache(paths, dataclasses.replace(check, declined=False), output)
    if staged is None:
        return False
    output(f"Support plane staged {staged}; re-exec requested")
    return True


def _automatic_update_offer(*, paths: ManagedPaths, installed_version: str,
                            home: Path | None, platform: Platform,
                            transport: ReleaseTransport | None,
                            effective_uid: int | None, now: int,
                            interval: int, tty: Callable[[], bool],
                            prompt: Callable[[str], str],
                            output: Callable[[str], None]) -> bool:
    try:
        cached = _read_update_cache(paths)
    except DesktopCLIError:
        return False
    refreshed = False
    if (cached is not None and 0 <= now - cached.checked_at < interval):
        latest_version = cached.latest_version
    else:
        try:
            deadline = time.monotonic() + AUTOMATIC_RELEASE_TIMEOUT
            release = discover_official_release(
                transport=transport, timeout=AUTOMATIC_RELEASE_TIMEOUT,
                deadline=deadline,
            )
        except DesktopCLIError:
            return False
        latest_version = release.version
        refreshed = True
    if _version_tuple(latest_version) <= _version_tuple(installed_version):
        if refreshed:
            try:
                _write_update_cache(paths, latest_version, now)
            except DesktopCLIError:
                pass
        return False

    notice = (
        f"Update available: {latest_version} (installed {installed_version}); "
        "run 'lingtai-desktop update'."
    )
    try:
        interactive = bool(tty())
    except Exception:
        interactive = False
    if not interactive:
        if refreshed:
            try:
                _write_update_cache(paths, latest_version, now)
            except DesktopCLIError:
                pass
        output(notice)
        return False

    try:
        answer = prompt(
            f"LingTai Desktop {latest_version} is available "
            f"(installed {installed_version}). Update now? [y/N] "
        )
    except (EOFError, OSError):
        answer = ""
    if answer.strip().lower() not in {"y", "yes"}:
        if refreshed:
            try:
                _write_update_cache(paths, latest_version, now)
            except DesktopCLIError:
                pass
        return False

    try:
        updated_version = install_official(
            home=home, platform=platform, transport=transport,
            effective_uid=effective_uid, update=True,
        )
    except DesktopCLIError as error:
        output(
            f"Update failed; continuing with LingTai Desktop {installed_version}: {error}"
        )
        return False
    try:
        _write_update_cache(paths, updated_version, now)
    except DesktopCLIError as error:
        output(f"Updated to {updated_version}, but update-check cache was not recorded: {error}")
    else:
        output(f"Updated LingTai Desktop to {updated_version}")
    return True


def support_diagnostics(paths: ManagedPaths) -> dict[str, object]:
    active = _validate_owned_cli(paths)
    state, _ = _read_support_state(paths)
    pending_loaded = _read_support_pending(paths)
    generations: dict[str, ValidatedSupportGeneration] = {}
    for path in sorted(paths.support_versions.iterdir(), key=lambda item: item.name):
        if not _is_safe_support_generation(path.name):
            raise DesktopCLIError("support versions contains an unknown generation")
        generations[path.name] = validate_support_generation(path)
    pending_text = "none"
    referenced = {active.manifest.generation_id, state.last_good_generation}
    if pending_loaded is not None:
        pending, _ = pending_loaded
        pending_text = f"{pending.from_generation}->{pending.to_generation}"
        referenced.update((pending.from_generation, pending.to_generation))
    failed = [item.generation_id for item in state.failed_generations]
    orphans = sorted(set(generations) - referenced)
    return {
        "bootstrap_protocol": SUPPORT_BOOTSTRAP_PROTOCOL,
        "bootstrap_sha256": STABLE_BOOTSTRAP_SHA256,
        "active_generation": active.manifest.generation_id,
        "active_manifest_sha256": active.manifest.manifest_sha256,
        "high_water_version": state.high_water_version,
        "high_water_manifest_sha256": state.high_water_manifest_sha256,
        "last_good_generation": state.last_good_generation,
        "pending": pending_text,
        "failed_generations": failed,
        "orphan_generations": orphans,
    }


def doctor(*, home: Path | None = None) -> tuple[str, dict[str, object]]:
    paths = _paths(home)
    for path in (
            paths.local, paths.bin, paths.root, paths.support,
            paths.support_versions, paths.versions, paths.receipts):
        if path.is_symlink() or not path.is_dir():
            raise DesktopCLIError("managed layout contains a missing/non-directory/symlink root")
    support_diagnostics(paths)
    _read_update_cache(paths)
    version, _, receipt = _active(paths)
    source = receipt.get("source_archive")
    if not isinstance(source, dict) or set(source) != {"file_name", "size_bytes", "sha256"}:
        raise DesktopCLIError("receipt source artifact facts are invalid")
    return version, receipt


def run_installed(arguments: Sequence[str], *, home: Path | None = None,
                  platform: Platform | None = None,
                  transport: ReleaseTransport | None = None,
                  effective_uid: int | None = None,
                  clock: Callable[[], float] | None = None,
                  check_interval: int = DEFAULT_UPDATE_CHECK_INTERVAL,
                  support_check_interval: int = DEFAULT_SUPPORT_UPDATE_CHECK_INTERVAL,
                  tty: Callable[[], bool] | None = None,
                  prompt: Callable[[str], str] | None = None,
                  exec_launcher: Callable[[Path, list[str], dict[str, str]], object] | None = None,
                  skip_support_check: bool = False,
                  output: Callable[[str], None] = print) -> int:
    platform = platform or Platform()
    clock = clock or time.time
    tty = tty or _default_tty
    prompt = prompt or input
    paths = _paths(home)
    command = "open" if not arguments else arguments[0]
    rest = list(arguments[1:])
    if command in {"open", "version", "doctor"} and rest:
        raise DesktopCLIError(f"{command} takes no arguments")
    if command in {"open", "foreground", "version", "doctor"}:
        version, app, receipt = _active(paths)
        _validate_owned_cli(paths)
        now = max(0, int(clock()))
        if (not skip_support_check and _automatic_support_update_offer(
            paths=paths, arguments=arguments, home=home,
            transport=transport, effective_uid=effective_uid, now=now,
            interval=support_check_interval, tty=tty, prompt=prompt,
            output=output, exec_launcher=exec_launcher,
        )):
            # A real executor never returns. An injected executor represents the
            # handoff and must not run either the old command or the App offer.
            return 0
        if _automatic_update_offer(
            paths=paths, installed_version=version, home=home,
            platform=platform, transport=transport,
            effective_uid=effective_uid, now=now,
            interval=check_interval, tty=tty, prompt=prompt,
            output=output,
        ):
            version, app, receipt = _active(paths)
            _validate_owned_cli(paths)
        if command == "open":
            platform.open_app(app)
        elif command == "foreground":
            if rest[:1] == ["--"]:
                rest = rest[1:]
            platform.exec_app(app / "Contents/MacOS/LingTai", rest)
        elif command == "version":
            output(f"version: {version}")
            output(f"archive sha256: {receipt['source_archive']['sha256']}")  # type: ignore[index]
            output(f"bundle sha256: {receipt['bundle_tree_sha256']}")
        else:
            version, receipt = doctor(home=home)
            diagnostics = support_diagnostics(paths)
            output(f"INTEGRITY PASS: managed LingTai Desktop {version}")
            output(f"archive binding: {receipt['source_archive']['sha256']}")  # type: ignore[index]
            output(f"bootstrap protocol: {diagnostics['bootstrap_protocol']}")
            output(f"bootstrap sha256: {diagnostics['bootstrap_sha256']}")
            output(f"support generation: {diagnostics['active_generation']}")
            output(f"support manifest sha256: {diagnostics['active_manifest_sha256']}")
            output(f"support high-water: {diagnostics['high_water_version']}")
            output(f"support last-good: {diagnostics['last_good_generation']}")
            output(f"support pending: {diagnostics['pending']}")
            output("support failed: " + (", ".join(diagnostics["failed_generations"]) or "none"))  # type: ignore[arg-type]
            output("support orphans: " + (", ".join(diagnostics["orphan_generations"]) or "none"))  # type: ignore[arg-type]
        return 0
    if command == "update":
        parser = argparse.ArgumentParser(prog="lingtai-desktop update")
        parser.add_argument("--version")
        parser.add_argument("--archive", type=Path)
        parser.add_argument("--manifest", type=Path)
        values = parser.parse_args(rest)
        local_pair = values.archive is not None or values.manifest is not None
        if (values.archive is None) != (values.manifest is None):
            raise DesktopCLIError("--archive and --manifest must be supplied together")
        if local_pair and values.version is not None:
            raise DesktopCLIError("--version is mutually exclusive with --archive/--manifest")
        if local_pair:
            version = install(
                values.archive, values.manifest, home=home, platform=platform,
                effective_uid=effective_uid, update=True,
            )
        else:
            if not skip_support_check:
                try:
                    _read_support_update_cache(paths)
                    staged_support = stage_official_support_update(
                        values.version,
                        argv=[os.fspath(paths.launcher), *list(arguments)],
                        home=home, transport=transport,
                        effective_uid=effective_uid, explicit_retry=True,
                        exec_launcher=exec_launcher,
                    )
                except DesktopCLIError as error:
                    output(f"Support update failed; continuing App update: {error}")
                else:
                    if staged_support is not None:
                        output(f"Support plane staged {staged_support}; re-exec requested")
                        return 0
                    output("Support plane is already current; continuing App update")
            else:
                output("Support plane re-exec guard consumed; continuing App update")
            _read_update_cache(paths)
            version = install_official(
                values.version, home=home, platform=platform,
                transport=transport, effective_uid=effective_uid, update=True,
            )
            try:
                _write_update_cache(paths, version, max(0, int(clock())))
            except DesktopCLIError as error:
                output(
                    f"updated LingTai Desktop to {version}, but update-check "
                    f"cache was not recorded: {error}"
                )
                return 0
        output(f"App plane updated to {version}")
        output(f"updated LingTai Desktop to {version}")
        return 0
    if command == "uninstall":
        parser = argparse.ArgumentParser(prog="lingtai-desktop uninstall")
        group = parser.add_mutually_exclusive_group(required=True)
        group.add_argument("--version")
        group.add_argument("--all", action="store_true")
        values = parser.parse_args(rest)
        if values.all:
            uninstall_all(home=home, effective_uid=effective_uid)
        else:
            uninstall_version(values.version, home=home, effective_uid=effective_uid)
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
    app_plan = _preflight_uninstall(paths)
    support_plan = _preflight_support_uninstall(paths)

    # Both independent planes are completely proven before the first deletion.
    for entry in app_plan.entries:
        _revalidate_uninstall_ancestors(paths, app_plan)
        _remove_owned_version(
            paths.versions / entry.version,
            entry.receipt["bundle_tree_sha256"],  # type: ignore[arg-type]
            entry.version_identity,
        )
        receipt_path = paths.receipts / f"{entry.version}.json"
        _revalidate_uninstall_ancestors(paths, app_plan)
        if not _matches_identity(receipt_path, entry.receipt_identity):
            raise DesktopCLIError("refusing to remove a replaced receipt")
        receipt_path.unlink()
    _revalidate_uninstall_ancestors(paths, app_plan)
    _unlink_if_identity(paths.current, app_plan.current_identity)
    _unlink_if_identity(paths.update_cache, app_plan.update_cache_identity)

    _revalidate_support_uninstall(paths, support_plan)
    _unlink_if_identity(paths.support_current, support_plan.current_identity)
    _unlink_if_identity(paths.support_pending, support_plan.pending_identity)
    if support_plan.rollback_pointer_name is not None:
        _unlink_if_identity(
            paths.support / support_plan.rollback_pointer_name,
            support_plan.rollback_pointer_identity,
        )
    _unlink_if_identity(paths.support_update_cache, support_plan.update_cache_identity)
    _unlink_if_identity(paths.support_state, support_plan.state_identity)
    for generation in support_plan.generations:
        _revalidate_support_uninstall(paths, support_plan)
        _remove_validated_support_generation(generation)
    _unlink_if_identity(paths.launcher, support_plan.launcher_identity)

    for directory, identity in (
        (paths.support_versions, support_plan.versions_identity),
        (paths.support, support_plan.support_identity),
        (paths.receipts, app_plan.receipts_identity),
        (paths.versions, app_plan.versions_identity),
        (paths.root, app_plan.root_identity),
    ):
        if not _matches_identity(directory, identity):
            raise DesktopCLIError(f"refusing to remove replaced managed directory: {directory.name}")
        try:
            directory.rmdir()
        except OSError as error:
            raise DesktopCLIError(
                f"refusing to remove non-empty managed directory: {directory.name}"
            ) from error


def installed_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="lingtai-desktop",
        description="Launch or manage the user-level LingTai Desktop installation.")
    parser.add_argument("arguments", nargs=argparse.REMAINDER,
        help="open | foreground [-- APP_ARGS] | version | doctor | update ... | uninstall ...")
    return parser


def installed_main(argv: Sequence[str] | None = None) -> int:
    values = installed_parser().parse_args(argv)
    try:
        return run_installed(
            values.arguments, skip_support_check=_SUPPORT_REEXEC_CONSUMED,
        )
    except DesktopCLIError as error:
        print(f"lingtai-desktop: {error}", file=sys.stderr)
        return 1


def bootstrap_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Install LingTai Desktop into the current user's managed layout.")
    parser.add_argument("--version")
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--manifest", type=Path)
    return parser


def bootstrap_main(argv: Sequence[str] | None = None, *, home: Path | None = None,
                   platform: Platform | None = None,
                   transport: ReleaseTransport | None = None,
                   effective_uid: int | None = None,
                   output: Callable[[str], None] | None = None) -> int:
    values = bootstrap_parser().parse_args(argv)
    output = output or print
    try:
        local_pair = values.archive is not None or values.manifest is not None
        if (values.archive is None) != (values.manifest is None):
            raise DesktopCLIError("--archive and --manifest must be supplied together")
        if local_pair and values.version is not None:
            raise DesktopCLIError("--version is mutually exclusive with --archive/--manifest")
        if local_pair:
            version = install(
                values.archive, values.manifest, home=home, platform=platform,
                effective_uid=effective_uid,
            )
        else:
            version = install_official(
                values.version, home=home, platform=platform,
                transport=transport, effective_uid=effective_uid,
            )
        doctor(home=home)
    except DesktopCLIError as error:
        print(f"install-macos-app: {error}", file=sys.stderr)
        return 1
    output(f"installed LingTai Desktop {version}")
    output("launcher: $HOME/.local/bin/lingtai-desktop (PATH was not modified)")
    return 0


if __name__ == "__main__":
    raise SystemExit(installed_main())
