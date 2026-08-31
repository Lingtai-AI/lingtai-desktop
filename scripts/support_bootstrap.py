#!/usr/bin/env python3
# lingtai-desktop-support-bootstrap-v1
"""Stable local validator/switcher for managed LingTai Desktop support generations.

This bootstrap deliberately owns no release discovery or App lifecycle policy. It
validates the complete local support chain before importing an active CLI, resumes
one pending pointer transaction, and delegates the unchanged command.
"""

from __future__ import annotations

import dataclasses
import hashlib
import importlib.util
import json
import os
import re
import stat
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path
from typing import Callable, Mapping, Sequence


LAUNCHER_MARKER = "# lingtai-desktop-support-bootstrap-v1"
SUPPORT_MANIFEST_SCHEMA = "lingtai.desktop.support/v1"
SUPPORT_STATE_SCHEMA = "lingtai.desktop.support-state/v1"
SUPPORT_PENDING_SCHEMA = "lingtai.desktop.support-pending/v1"
SUPPORT_BOOTSTRAP_PROTOCOL = 1
SUPPORT_REPOSITORY = "Lingtai-AI/lingtai-desktop"
SUPPORT_REEXEC_MARKER = "LINGTAI_DESKTOP_SUPPORT_REEXEC"
SUPPORT_MANIFEST_NAME = "support-manifest.json"
SUPPORT_PAYLOAD_NAMES = ("desktop_user_cli.py", "verify-app-archive.py")
SUPPORT_PAYLOAD_MODE = 0o600
SUPPORT_GENERATION_MODE = 0o700
SUPPORT_GENERATION_DIGEST_LENGTH = 12
MAX_VERSION_COMPONENT_DIGITS = 9
MAX_VERSION_LENGTH = 3 * MAX_VERSION_COMPONENT_DIGITS + 2
MAX_SUPPORT_MANIFEST_BYTES = 16 * 1024
MAX_SUPPORT_STATE_BYTES = 16 * 1024
MAX_SUPPORT_PENDING_BYTES = 4 * 1024
MAX_SUPPORT_PAYLOAD_BYTES = 2 * 1024 * 1024
MAX_LAUNCHER_BYTES = 256 * 1024
MAX_FAILED_SUPPORT_GENERATIONS = 32
SUPPORT_SELF_TEST_TIMEOUT = 5
VERSION_PATTERN = re.compile(
    rf"[0-9]{{1,{MAX_VERSION_COMPONENT_DIGITS}}}\."
    rf"[0-9]{{1,{MAX_VERSION_COMPONENT_DIGITS}}}\."
    rf"[0-9]{{1,{MAX_VERSION_COMPONENT_DIGITS}}}"
)
GENERATION_PATTERN = re.compile(
    rf"([0-9]{{1,{MAX_VERSION_COMPONENT_DIGITS}}}\."
    rf"[0-9]{{1,{MAX_VERSION_COMPONENT_DIGITS}}}\."
    rf"[0-9]{{1,{MAX_VERSION_COMPONENT_DIGITS}}})-"
    rf"([0-9a-f]{{{SUPPORT_GENERATION_DIGEST_LENGTH}}})"
)
SHA_PATTERN = re.compile(r"[0-9a-f]{64}")
MANIFEST_KEYS = {
    "schema", "support_version", "generation_id", "release_tag",
    "repository", "bootstrap_protocol", "minimum_bootstrap_protocol", "files",
}
FILE_KEYS = {"name", "size", "mode", "sha256"}
STATE_KEYS = {
    "schema", "high_water_version", "high_water_manifest_sha256",
    "last_good_generation", "failed_generations",
}
FAILED_KEYS = {"generation_id", "manifest_sha256"}
PENDING_KEYS = {
    "schema", "from_generation", "to_generation", "to_manifest_sha256",
    "expected_current_dev", "expected_current_ino", "requested_argv_sha256",
}
_FAILPOINT: str | None = None  # Tests inject process-death boundaries in-process.


class BootstrapError(RuntimeError):
    """A bounded local bootstrap failure safe to print."""


class InjectedCrash(BaseException):
    """Test-only process-death analogue; normal rollback handlers do not catch it."""


@dataclasses.dataclass(frozen=True)
class SupportPaths:
    home: Path
    local: Path
    bin: Path
    share: Path
    root: Path
    support: Path
    versions: Path
    current: Path
    pending: Path
    state: Path
    update_cache: Path
    launcher: Path


@dataclasses.dataclass(frozen=True)
class Generation:
    path: Path
    support_version: str
    generation_id: str
    manifest_sha256: str
    directory_identity: tuple[int, int]
    file_identities: tuple[tuple[str, tuple[int, int]], ...]


@dataclasses.dataclass(frozen=True)
class Pending:
    from_generation: str
    to_generation: str
    to_manifest_sha256: str
    expected_current_dev: int
    expected_current_ino: int
    requested_argv_sha256: str


@dataclasses.dataclass(frozen=True)
class FailedGeneration:
    generation_id: str
    manifest_sha256: str


@dataclasses.dataclass(frozen=True)
class State:
    high_water_version: str
    high_water_manifest_sha256: str
    last_good_generation: str
    failed_generations: tuple[FailedGeneration, ...]


def _trip(name: str) -> None:
    if _FAILPOINT == name:
        raise InjectedCrash(name)


def _is_version(value: object) -> bool:
    return (
        isinstance(value, str) and len(value) <= MAX_VERSION_LENGTH
        and value.isascii() and VERSION_PATTERN.fullmatch(value) is not None
    )


def _version_tuple(value: str) -> tuple[int, int, int]:
    if not _is_version(value):
        raise BootstrapError("support version is malformed")
    return tuple(int(part) for part in value.split("."))  # type: ignore[return-value]


def _is_generation(value: object) -> bool:
    return (
        isinstance(value, str)
        and len(value) <= MAX_VERSION_LENGTH + 1 + SUPPORT_GENERATION_DIGEST_LENGTH
        and value.isascii() and GENERATION_PATTERN.fullmatch(value) is not None
    )


def _exact_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate key")
        result[key] = value
    return result


def _canonical(value: object, limit: int, label: str) -> bytes:
    try:
        payload = (json.dumps(
            value, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
        ) + "\n").encode("ascii")
    except (TypeError, ValueError, UnicodeError) as error:
        raise BootstrapError(f"{label} is not canonical JSON") from error
    if len(payload) > limit:
        raise BootstrapError(f"{label} is too large")
    return payload


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _paths(home: Path | None) -> SupportPaths:
    raw = Path(home) if home is not None else Path(os.environ.get("HOME", ""))
    if not os.fspath(raw) or not raw.is_absolute():
        raise BootstrapError("HOME must be present and absolute")
    _real_directory(raw, "HOME")
    local = raw / ".local"
    share = local / "share"
    root = share / "lingtai-desktop"
    support = root / "support"
    return SupportPaths(
        raw, local, local / "bin", share, root, support,
        support / "versions", support / "current", support / "pending.json",
        support / "state.json", support / "update-check.json",
        local / "bin/lingtai-desktop",
    )


def _real_directory(path: Path, label: str, mode: int | None = None) -> os.stat_result:
    try:
        facts = path.lstat()
    except OSError as error:
        raise BootstrapError(f"{label} is missing") from error
    if stat.S_ISLNK(facts.st_mode) or not stat.S_ISDIR(facts.st_mode):
        raise BootstrapError(f"{label} is not a real directory")
    if mode is not None and stat.S_IMODE(facts.st_mode) != mode:
        raise BootstrapError(f"{label} mode is invalid")
    return facts


def _read_file(path: Path, label: str, limit: int, *, mode: int,
               expected_size: int | None = None) -> tuple[bytes, tuple[int, int]]:
    descriptor: int | None = None
    try:
        descriptor = os.open(
            path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
            | getattr(os, "O_NONBLOCK", 0),
        )
        facts = os.fstat(descriptor)
        if (not stat.S_ISREG(facts.st_mode) or facts.st_nlink != 1
                or stat.S_IMODE(facts.st_mode) != mode
                or facts.st_size <= 0 or facts.st_size > limit
                or (expected_size is not None and facts.st_size != expected_size)):
            raise BootstrapError(f"{label} ownership, mode, or size is invalid")
        identity = (facts.st_dev, facts.st_ino)
        with os.fdopen(descriptor, "rb") as stream:
            descriptor = None
            value = stream.read(limit + 1)
    except BootstrapError:
        raise
    except OSError as error:
        raise BootstrapError(f"{label} is not a safe regular file") from error
    finally:
        if descriptor is not None:
            os.close(descriptor)
    if len(value) > limit or (expected_size is not None and len(value) != expected_size):
        raise BootstrapError(f"{label} size changed while reading")
    return value, identity


def _manifest(path: Path) -> tuple[dict[str, object], bytes, tuple[int, int]]:
    raw, identity = _read_file(
        path, "support manifest", MAX_SUPPORT_MANIFEST_BYTES,
        mode=SUPPORT_PAYLOAD_MODE,
    )
    try:
        value = json.loads(raw, object_pairs_hook=_exact_object)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError, ValueError) as error:
        raise BootstrapError("support manifest is not bounded valid JSON") from error
    if (not isinstance(value, dict) or set(value) != MANIFEST_KEYS
            or value.get("schema") != SUPPORT_MANIFEST_SCHEMA):
        raise BootstrapError("support manifest does not match the exact schema")
    version = value.get("support_version")
    generation = value.get("generation_id")
    release_tag = value.get("release_tag")
    protocol = value.get("bootstrap_protocol")
    minimum = value.get("minimum_bootstrap_protocol")
    if (not _is_version(version) or not _is_generation(generation)
            or not isinstance(release_tag, str) or not release_tag.startswith("v")
            or not _is_version(release_tag[1:])
            or value.get("repository") != SUPPORT_REPOSITORY):
        raise BootstrapError("support manifest release identity is invalid")
    if (type(protocol) is not int or protocol != SUPPORT_BOOTSTRAP_PROTOCOL
            or type(minimum) is not int or minimum < 1):
        raise BootstrapError("support bootstrap protocol declaration is invalid")
    if minimum > SUPPORT_BOOTSTRAP_PROTOCOL:
        raise BootstrapError("support generation requires a newer stable bootstrap protocol")
    raw_files = value.get("files")
    if not isinstance(raw_files, list) or len(raw_files) != len(SUPPORT_PAYLOAD_NAMES):
        raise BootstrapError("support manifest payload set is not exact")
    files: list[dict[str, object]] = []
    for expected, item in zip(SUPPORT_PAYLOAD_NAMES, raw_files):
        if not isinstance(item, dict) or set(item) != FILE_KEYS:
            raise BootstrapError("support manifest payload entry is invalid")
        if item.get("name") != expected:
            raise BootstrapError("support manifest payload names or order are not exact")
        size, mode, digest = item.get("size"), item.get("mode"), item.get("sha256")
        if (type(size) is not int or size <= 0 or size > MAX_SUPPORT_PAYLOAD_BYTES
                or type(mode) is not int or mode != SUPPORT_PAYLOAD_MODE
                or not isinstance(digest, str) or SHA_PATTERN.fullmatch(digest) is None):
            raise BootstrapError("support manifest payload facts are invalid")
        files.append(dict(item))
    identity_value = {
        "bootstrap_protocol": protocol,
        "files": files,
        "minimum_bootstrap_protocol": minimum,
        "release_tag": release_tag,
        "repository": SUPPORT_REPOSITORY,
        "schema": SUPPORT_MANIFEST_SCHEMA,
        "support_version": version,
    }
    derived = f"{version}-{_sha(_canonical(identity_value, MAX_SUPPORT_MANIFEST_BYTES, 'support manifest identity'))[:SUPPORT_GENERATION_DIGEST_LENGTH]}"
    if generation != derived:
        raise BootstrapError("support generation identity does not match canonical manifest content")
    canonical = dict(identity_value)
    canonical["generation_id"] = generation
    canonical_bytes = _canonical(canonical, MAX_SUPPORT_MANIFEST_BYTES, "support manifest")
    if raw != canonical_bytes:
        raise BootstrapError("support manifest bytes are not canonical")
    value["files"] = files
    return value, canonical_bytes, identity


def _validate_generation(paths: SupportPaths, generation_id: str) -> Generation:
    if not _is_generation(generation_id):
        raise BootstrapError("support generation identity is malformed")
    path = paths.versions / generation_id
    directory = _real_directory(path, "support generation", SUPPORT_GENERATION_MODE)
    try:
        actual = {entry.name for entry in os.scandir(path)}
    except OSError as error:
        raise BootstrapError("support generation could not be enumerated") from error
    if actual != {SUPPORT_MANIFEST_NAME, *SUPPORT_PAYLOAD_NAMES}:
        raise BootstrapError("support generation file set is not exact")
    value, manifest_bytes, manifest_identity = _manifest(path / SUPPORT_MANIFEST_NAME)
    if value["generation_id"] != generation_id:
        raise BootstrapError("support generation directory does not match its manifest")
    identities: list[tuple[str, tuple[int, int]]] = [
        (SUPPORT_MANIFEST_NAME, manifest_identity),
    ]
    for item in value["files"]:  # type: ignore[assignment]
        name = item["name"]  # type: ignore[index]
        content, identity = _read_file(
            path / name, f"support payload {name}", MAX_SUPPORT_PAYLOAD_BYTES,
            mode=item["mode"], expected_size=item["size"],  # type: ignore[index,arg-type]
        )
        if _sha(content) != item["sha256"]:  # type: ignore[index]
            raise BootstrapError(f"support payload {name} SHA-256 does not match manifest")
        identities.append((name, identity))
    return Generation(
        path, value["support_version"], generation_id, _sha(manifest_bytes),  # type: ignore[arg-type]
        (directory.st_dev, directory.st_ino), tuple(identities),
    )


def _state_value(state: State) -> dict[str, object]:
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


def _parse_state(raw: bytes) -> State:
    try:
        value = json.loads(raw, object_pairs_hook=_exact_object)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError, ValueError) as error:
        raise BootstrapError("support state is not bounded valid JSON") from error
    if (not isinstance(value, dict) or set(value) != STATE_KEYS
            or value.get("schema") != SUPPORT_STATE_SCHEMA):
        raise BootstrapError("support state does not match the exact schema")
    version = value.get("high_water_version")
    digest = value.get("high_water_manifest_sha256")
    last_good = value.get("last_good_generation")
    if (not _is_version(version) or not isinstance(digest, str)
            or SHA_PATTERN.fullmatch(digest) is None or not _is_generation(last_good)):
        raise BootstrapError("support state identities are invalid")
    raw_failed = value.get("failed_generations")
    if not isinstance(raw_failed, list) or len(raw_failed) > MAX_FAILED_SUPPORT_GENERATIONS:
        raise BootstrapError("support state failed-generation list is invalid")
    failed: list[FailedGeneration] = []
    seen: set[str] = set()
    for item in raw_failed:
        if not isinstance(item, dict) or set(item) != FAILED_KEYS:
            raise BootstrapError("support failed-generation entry is invalid")
        generation, failed_digest = item.get("generation_id"), item.get("manifest_sha256")
        if (not _is_generation(generation) or generation in seen
                or not isinstance(failed_digest, str)
                or SHA_PATTERN.fullmatch(failed_digest) is None):
            raise BootstrapError("support failed-generation identity is invalid")
        seen.add(generation)
        failed.append(FailedGeneration(generation, failed_digest))
    state = State(version, digest, last_good, tuple(failed))
    if raw != _canonical(_state_value(state), MAX_SUPPORT_STATE_BYTES, "support state"):
        raise BootstrapError("support state bytes are not canonical")
    return state


def _load_state(paths: SupportPaths) -> tuple[State, tuple[int, int]]:
    raw, identity = _read_file(
        paths.state, "support state", MAX_SUPPORT_STATE_BYTES,
        mode=SUPPORT_PAYLOAD_MODE,
    )
    return _parse_state(raw), identity


def _pending_value(pending: Pending) -> dict[str, object]:
    return {
        "expected_current_dev": pending.expected_current_dev,
        "expected_current_ino": pending.expected_current_ino,
        "from_generation": pending.from_generation,
        "requested_argv_sha256": pending.requested_argv_sha256,
        "schema": SUPPORT_PENDING_SCHEMA,
        "to_generation": pending.to_generation,
        "to_manifest_sha256": pending.to_manifest_sha256,
    }


def _parse_pending(raw: bytes) -> Pending:
    try:
        value = json.loads(raw, object_pairs_hook=_exact_object)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError, ValueError) as error:
        raise BootstrapError("support pending journal is not bounded valid JSON") from error
    if (not isinstance(value, dict) or set(value) != PENDING_KEYS
            or value.get("schema") != SUPPORT_PENDING_SCHEMA):
        raise BootstrapError("support pending journal does not match the exact schema")
    source, target = value.get("from_generation"), value.get("to_generation")
    digest, argv_digest = value.get("to_manifest_sha256"), value.get("requested_argv_sha256")
    dev, ino = value.get("expected_current_dev"), value.get("expected_current_ino")
    if (not _is_generation(source) or not _is_generation(target) or source == target
            or not isinstance(digest, str) or SHA_PATTERN.fullmatch(digest) is None
            or not isinstance(argv_digest, str) or SHA_PATTERN.fullmatch(argv_digest) is None
            or type(dev) is not int or dev < 0 or type(ino) is not int or ino < 0):
        raise BootstrapError("support pending journal identities are invalid")
    pending = Pending(source, target, digest, dev, ino, argv_digest)
    if raw != _canonical(_pending_value(pending), MAX_SUPPORT_PENDING_BYTES, "support pending journal"):
        raise BootstrapError("support pending journal bytes are not canonical")
    return pending


def _load_pending(paths: SupportPaths) -> tuple[Pending, tuple[int, int]] | None:
    if not paths.pending.exists() and not paths.pending.is_symlink():
        return None
    raw, identity = _read_file(
        paths.pending, "support pending journal", MAX_SUPPORT_PENDING_BYTES,
        mode=SUPPORT_PAYLOAD_MODE,
    )
    return _parse_pending(raw), identity


def _current(paths: SupportPaths) -> tuple[str, tuple[int, int]]:
    try:
        facts = paths.current.lstat()
    except OSError as error:
        raise BootstrapError("support current pointer is missing") from error
    if not stat.S_ISLNK(facts.st_mode) or facts.st_nlink != 1:
        raise BootstrapError("support current pointer is not a single-link symlink")
    try:
        target = os.readlink(paths.current)
    except OSError as error:
        raise BootstrapError("support current pointer could not be read") from error
    prefix = "versions/"
    generation = target[len(prefix):] if target.startswith(prefix) else ""
    if not _is_generation(generation) or target != f"versions/{generation}":
        raise BootstrapError("support current pointer escapes or is malformed")
    return generation, (facts.st_dev, facts.st_ino)


def _validate_launcher(paths: SupportPaths) -> None:
    installed, _ = _read_file(
        paths.launcher, "stable support bootstrap", MAX_LAUNCHER_BYTES, mode=0o755,
    )
    try:
        expected = Path(__file__).read_bytes()
    except OSError as error:
        raise BootstrapError("stable bootstrap could not validate its own source") from error
    if installed != expected or LAUNCHER_MARKER.encode() not in installed[:512]:
        raise BootstrapError("stable support bootstrap identity is invalid")


def _layout(paths: SupportPaths) -> None:
    for path, label in (
        (paths.local, "managed .local"), (paths.bin, "managed bin"),
        (paths.share, "managed share"), (paths.root, "managed root"),
    ):
        _real_directory(path, label)
    _real_directory(paths.support, "managed support", 0o700)
    _real_directory(paths.versions, "managed support versions", 0o700)
    _validate_launcher(paths)


def _fsync_directory(path: Path) -> None:
    descriptor: int | None = None
    try:
        descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        os.fsync(descriptor)
    except OSError as error:
        raise BootstrapError(f"could not durably publish {path.name}") from error
    finally:
        if descriptor is not None:
            os.close(descriptor)


def _matches(path: Path, identity: tuple[int, int]) -> bool:
    try:
        facts = path.lstat()
    except OSError:
        return False
    return (facts.st_dev, facts.st_ino) == identity


def _atomic_write(path: Path, payload: bytes, previous: tuple[int, int], label: str) -> tuple[int, int]:
    temporary = path.parent / f".{path.name}-{uuid.uuid4().hex}"
    descriptor: int | None = None
    temporary_identity: tuple[int, int] | None = None
    try:
        descriptor = os.open(
            temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL
            | getattr(os, "O_NOFOLLOW", 0), 0o600,
        )
        with os.fdopen(descriptor, "wb") as stream:
            descriptor = None
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o600, follow_symlinks=False)
        staged, temporary_identity = _read_file(
            temporary, f"staged {label}", max(len(payload), 1), mode=0o600,
            expected_size=len(payload),
        )
        if staged != payload or not _matches(path, previous):
            raise BootstrapError(f"refusing to replace raced {label}")
        os.replace(temporary, path)
        if not _matches(path, temporary_identity):
            raise BootstrapError(f"{label} publication was replaced")
        _fsync_directory(path.parent)
        return temporary_identity
    except BootstrapError:
        raise
    except OSError as error:
        raise BootstrapError(f"could not publish {label}") from error
    finally:
        if descriptor is not None:
            os.close(descriptor)
        if temporary_identity is not None and _matches(temporary, temporary_identity):
            temporary.unlink()


def _replace_current(paths: SupportPaths, generation: str,
                     observed_identity: tuple[int, int]) -> tuple[int, int]:
    if not _matches(paths.current, observed_identity):
        raise BootstrapError("refusing to replace raced support current pointer")
    temporary = paths.support / f".current-{uuid.uuid4().hex}"
    try:
        os.symlink(f"versions/{generation}", temporary)
        temporary_identity = (
            temporary.lstat().st_dev, temporary.lstat().st_ino,
        )
        _fsync_directory(paths.support)
        _trip("pointer-temporary")
        if not _matches(paths.current, observed_identity):
            raise BootstrapError("refusing to replace raced support current pointer")
        os.replace(temporary, paths.current)
        if not _matches(paths.current, temporary_identity):
            raise BootstrapError("support current publication was replaced")
        _fsync_directory(paths.support)
        _trip("pointer-replaced")
        return temporary_identity
    except BootstrapError:
        raise
    except OSError as error:
        raise BootstrapError("could not switch support current pointer") from error
    finally:
        if temporary.is_symlink():
            temporary.unlink()


def _candidate_policy(candidate: Generation, state: State, *, explicit_retry: bool = False) -> None:
    candidate_version = _version_tuple(candidate.support_version)
    high_water = _version_tuple(state.high_water_version)
    if candidate_version < high_water:
        raise BootstrapError("support update target is below the high-water mark")
    if candidate_version == high_water and candidate.manifest_sha256 != state.high_water_manifest_sha256:
        raise BootstrapError("support update target substitutes a same-version manifest")
    if not explicit_retry and any(
            item.generation_id == candidate.generation_id
            and item.manifest_sha256 == candidate.manifest_sha256
            for item in state.failed_generations):
        raise BootstrapError("support update target was already recorded as failed")


def _production_self_test(generation: Generation) -> None:
    code = (
        "import importlib.util,sys;"
        "p=sys.argv[1];"
        "s=importlib.util.spec_from_file_location('lingtai_support_self_test',p);"
        "m=importlib.util.module_from_spec(s);"
        "sys.modules[s.name]=m;"
        "s.loader.exec_module(m);"
        "r=m.support_self_test();"
        "raise SystemExit(0 if r is True else 1)"
    )
    environment = dict(os.environ)
    environment.pop(SUPPORT_REEXEC_MARKER, None)
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    try:
        result = subprocess.run(
            [sys.executable, "-I", "-B", "-c", code,
             os.fspath(generation.path / "desktop_user_cli.py")],
            env=environment, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, timeout=SUPPORT_SELF_TEST_TIMEOUT, check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise BootstrapError("support self-test timed out") from error
    except OSError as error:
        raise BootstrapError("support self-test could not start") from error
    if result.returncode:
        raise BootstrapError("support import or self-test failed")


def _unlink_pending(paths: SupportPaths, identity: tuple[int, int]) -> None:
    if not _matches(paths.pending, identity):
        raise BootstrapError("refusing to remove raced support pending journal")
    try:
        paths.pending.unlink()
    except OSError as error:
        raise BootstrapError("could not remove support pending journal") from error
    _fsync_directory(paths.support)
    _trip("pending-removed")


def _commit_target(paths: SupportPaths, state: State, state_identity: tuple[int, int],
                   target: Generation, pending_identity: tuple[int, int]) -> None:
    high_water_version = state.high_water_version
    high_water_digest = state.high_water_manifest_sha256
    if _version_tuple(target.support_version) >= _version_tuple(high_water_version):
        high_water_version = target.support_version
        high_water_digest = target.manifest_sha256
    committed = State(
        high_water_version, high_water_digest, target.generation_id,
        tuple(item for item in state.failed_generations
              if item.generation_id != target.generation_id),
    )
    _atomic_write(
        paths.state, _canonical(_state_value(committed), MAX_SUPPORT_STATE_BYTES, "support state"),
        state_identity, "support state",
    )
    _trip("state-published")
    _unlink_pending(paths, pending_identity)


def _rollback_target(paths: SupportPaths, state: State, state_identity: tuple[int, int],
                     source: Generation, target: Generation,
                     current_generation: str, current_identity: tuple[int, int],
                     pending_identity: tuple[int, int]) -> None:
    if current_generation == target.generation_id:
        _replace_current(paths, source.generation_id, current_identity)
        _trip("rollback-pointer-replaced")
    elif current_generation != source.generation_id:
        raise BootstrapError("support rollback observed an unrelated current pointer")
    failed = list(state.failed_generations)
    replacement = FailedGeneration(target.generation_id, target.manifest_sha256)
    failed = [item for item in failed if item.generation_id != target.generation_id]
    failed.append(replacement)
    failed = failed[-MAX_FAILED_SUPPORT_GENERATIONS:]
    rolled_back = State(
        state.high_water_version, state.high_water_manifest_sha256,
        source.generation_id, tuple(failed),
    )
    # If a pointer replacement occurred, reload state identity only; state itself
    # remains the already-validated high-water source of truth.
    if not _matches(paths.state, state_identity):
        raise BootstrapError("refusing to replace raced support state")
    _atomic_write(
        paths.state,
        _canonical(_state_value(rolled_back), MAX_SUPPORT_STATE_BYTES, "support state"),
        state_identity, "support state",
    )
    _trip("rollback-state-published")
    _unlink_pending(paths, pending_identity)


def process_pending(*, home: Path | None = None,
                    self_test_runner: Callable[[Generation], None] | None = None) -> Generation:
    paths = _paths(home)
    _layout(paths)
    state, state_identity = _load_state(paths)
    current_generation, current_identity = _current(paths)
    pending_loaded = _load_pending(paths)
    if pending_loaded is None:
        active = _validate_generation(paths, current_generation)
        if state.last_good_generation != active.generation_id:
            raise BootstrapError("support current and last-good state disagree without a transaction")
        return active

    pending, pending_identity = pending_loaded
    if current_generation not in {pending.from_generation, pending.to_generation}:
        raise BootstrapError("support pending transaction does not match current pointer")
    source = _validate_generation(paths, pending.from_generation)
    target = _validate_generation(paths, pending.to_generation)
    if target.manifest_sha256 != pending.to_manifest_sha256:
        raise BootstrapError("support pending target manifest digest is invalid")
    committed_recovery = (
        state.last_good_generation == target.generation_id
        and state.high_water_version == target.support_version
        and state.high_water_manifest_sha256 == target.manifest_sha256
    )
    if not committed_recovery and state.last_good_generation != source.generation_id:
        raise BootstrapError("support pending source is not the recorded last-good generation")
    recorded_failed = any(
        item.generation_id == target.generation_id
        and item.manifest_sha256 == target.manifest_sha256
        for item in state.failed_generations
    )
    if recorded_failed:
        if current_generation != source.generation_id:
            raise BootstrapError("failed support target remains selected")
        _unlink_pending(paths, pending_identity)
        return source
    _candidate_policy(target, state)
    if current_generation == source.generation_id:
        if current_identity != (pending.expected_current_dev, pending.expected_current_ino):
            raise BootstrapError("support pending current-pointer identity was replaced")
        current_identity = _replace_current(paths, target.generation_id, current_identity)
        current_generation = target.generation_id
    runner = self_test_runner or _production_self_test
    try:
        _trip("before-self-test")
        runner(target)
        _trip("after-self-test")
    except Exception:
        _rollback_target(
            paths, state, state_identity, source, target,
            current_generation, current_identity, pending_identity,
        )
        return _validate_generation(paths, source.generation_id)
    _commit_target(paths, state, state_identity, target, pending_identity)
    return _validate_generation(paths, target.generation_id)


def _validate_arguments(arguments: Sequence[str]) -> None:
    values = list(arguments)
    if not values:
        return
    command, rest = values[0], values[1:]
    if command in {"open", "version", "doctor"}:
        if rest:
            raise BootstrapError(f"{command} takes no arguments")
        return
    if command == "foreground":
        return
    if command == "uninstall":
        if rest == ["--all"]:
            return
        if len(rest) == 2 and rest[0] == "--version" and _is_version(rest[1]):
            return
        raise BootstrapError("uninstall requires exactly --all or --version x.y.z")
    if command == "update":
        seen: dict[str, str] = {}
        index = 0
        while index < len(rest):
            option = rest[index]
            if option not in {"--version", "--archive", "--manifest"} or option in seen:
                raise BootstrapError("update arguments are malformed")
            if index + 1 >= len(rest) or rest[index + 1].startswith("--"):
                raise BootstrapError("update option value is missing")
            seen[option] = rest[index + 1]
            index += 2
        if "--version" in seen and not _is_version(seen["--version"]):
            raise BootstrapError("update version is malformed")
        local = "--archive" in seen or "--manifest" in seen
        if local and not {"--archive", "--manifest"}.issubset(seen):
            raise BootstrapError("--archive and --manifest must be supplied together")
        if local and "--version" in seen:
            raise BootstrapError("--version is mutually exclusive with --archive/--manifest")
        return
    raise BootstrapError(
        f"unknown command: {command}; expected open, foreground, version, doctor, update, or uninstall"
    )


def _load_and_run(generation: Generation, arguments: Sequence[str]) -> int:
    module_path = generation.path / "desktop_user_cli.py"
    name = f"lingtai_desktop_active_{generation.generation_id.replace('-', '_')}"
    try:
        specification = importlib.util.spec_from_file_location(name, module_path)
        if specification is None or specification.loader is None:
            raise BootstrapError("active support CLI import specification is invalid")
        module = importlib.util.module_from_spec(specification)
        sys.modules[name] = module
        specification.loader.exec_module(module)
        entry = getattr(module, "installed_main")
    except BootstrapError:
        raise
    except Exception as error:
        raise BootstrapError("active support CLI import failed") from error
    if not callable(entry):
        raise BootstrapError("active support CLI entry point is invalid")
    return int(entry(list(arguments)))


def run_launcher(arguments: Sequence[str], *, home: Path | None = None,
                 self_test_runner: Callable[[Generation], None] | None = None,
                 installed_runner: Callable[[Path, Sequence[str]], int] | None = None) -> int:
    # Syntax is deliberately checked before pending state can be switched.
    _validate_arguments(arguments)
    os.environ.pop(SUPPORT_REEXEC_MARKER, None)
    active = process_pending(home=home, self_test_runner=self_test_runner)
    # Revalidate immediately before import; never search an arbitrary generation.
    active = _validate_generation(_paths(home), active.generation_id)
    if installed_runner is not None:
        return int(installed_runner(active.path / "desktop_user_cli.py", list(arguments)))
    return _load_and_run(active, arguments)


def main(argv: Sequence[str] | None = None) -> int:
    sys.dont_write_bytecode = True
    arguments = list(sys.argv[1:] if argv is None else argv)
    try:
        return run_launcher(arguments)
    except BootstrapError as error:
        print(f"lingtai-desktop: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
