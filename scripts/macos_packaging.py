#!/usr/bin/env python3
"""Fail-closed macOS packaging implementation for LingTai Desktop.

This module owns staging and release production.  Verification of the final
DMG intentionally lives in the separate verify-macos-package.py program.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Mapping, Sequence


APP_NAME = "LingTai.app"
BUNDLE_ID = "ai.lingtai.desktop"
MINIMUM_MACOS = "13.0"
REQUIRED_ARCHITECTURES = ("arm64", "x86_64")
VERSION_PATTERN = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")


class PackagingError(RuntimeError):
    """A bounded, secret-free packaging failure."""


@dataclasses.dataclass(frozen=True)
class ArtifactNames:
    dmg: str
    manifest: str


@dataclasses.dataclass(frozen=True)
class ReleaseCredentials:
    identity: str
    notary_profile: str


@dataclasses.dataclass(frozen=True)
class AppFacts:
    version: str
    architectures: tuple[str, ...]
    minimum_macos: str


def artifact_names(version: str) -> ArtifactNames:
    if VERSION_PATTERN.fullmatch(version) is None:
        raise PackagingError("unsafe version; expected three numeric components")
    stem = f"LingTai-{version}-macOS-universal"
    return ArtifactNames(dmg=f"{stem}.dmg", manifest=f"{stem}.manifest.json")


def resolve_release_credentials(
    mode: str,
    identity: str | None,
    notary_profile: str | None,
    environment: Mapping[str, str],
) -> ReleaseCredentials | None:
    if mode not in {"diagnostic", "release"}:
        raise PackagingError("mode must be exactly diagnostic or release")
    if mode == "diagnostic":
        if identity or notary_profile:
            raise PackagingError("diagnostic mode refuses release credentials")
        return None

    resolved_identity = identity or environment.get("LINGTAI_CODESIGN_IDENTITY")
    resolved_profile = notary_profile or environment.get("LINGTAI_NOTARY_PROFILE")
    if not resolved_identity:
        raise PackagingError(
            "release mode requires --signing-identity or "
            "LINGTAI_CODESIGN_IDENTITY"
        )
    if not resolved_profile:
        raise PackagingError(
            "release mode requires --notary-profile or LINGTAI_NOTARY_PROFILE"
        )
    if not resolved_identity.startswith("Developer ID Application:"):
        raise PackagingError("release mode requires a Developer ID Application identity")
    if any(character in resolved_identity for character in "\r\n\0"):
        raise PackagingError("Developer ID Application identity contains unsafe characters")
    if not resolved_profile.strip() or any(
        character in resolved_profile for character in "\r\n\0"
    ):
        raise PackagingError("notary profile name contains unsafe characters")
    return ReleaseCredentials(resolved_identity, resolved_profile)


def require_tool(name: str, environment: Mapping[str, str] = os.environ) -> Path:
    resolved = shutil.which(name, path=environment.get("PATH"))
    if not resolved:
        raise PackagingError(f"required tool is unavailable: {name}")
    return Path(resolved)


def parse_architectures(output: str) -> tuple[str, ...]:
    architectures = tuple(sorted(set(output.split())))
    if not architectures:
        raise PackagingError("lipo returned no architectures")
    return architectures


def parse_minos(output: str) -> dict[str, str]:
    facts: dict[str, str] = {}
    architecture: str | None = None
    for line in output.splitlines():
        match = re.search(r"\(architecture ([^)]+)\):$", line.strip())
        if match:
            architecture = match.group(1)
            continue
        match = re.fullmatch(r"\s*minos\s+([0-9]+(?:\.[0-9]+){1,2})\s*", line)
        if match and architecture:
            facts[architecture] = match.group(1)
    if not facts:
        raise PackagingError("vtool returned no macOS minimum-version facts")
    return facts


def _bounded_failure(label: str, result: subprocess.CompletedProcess[str]) -> str:
    output = (result.stdout or "").strip().replace("\0", "")
    if output:
        output = output[-1200:]
        return f"{label} failed (exit {result.returncode}): {output}"
    return f"{label} failed (exit {result.returncode})"


def run_checked(
    arguments: Sequence[str | os.PathLike[str]],
    *,
    label: str,
    environment: Mapping[str, str] | None = None,
    sensitive: bool = False,
) -> str:
    try:
        result = subprocess.run(
            [os.fspath(argument) for argument in arguments],
            env=dict(environment) if environment is not None else None,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
    except OSError as error:
        raise PackagingError(f"{label} could not start") from error
    if result.returncode != 0:
        if sensitive:
            raise PackagingError(f"{label} failed (exit {result.returncode})")
        raise PackagingError(_bounded_failure(label, result))
    return result.stdout or ""


def validate_output_paths(
    input_app: Path, output_directory: Path, names: ArtifactNames
) -> tuple[Path, Path]:
    app = input_app.resolve(strict=False)
    output = output_directory.resolve(strict=False)
    try:
        output.relative_to(app)
    except ValueError:
        pass
    else:
        raise PackagingError("output directory must not be inside the input App")
    if output == app.parent:
        raise PackagingError("output directory must not contain the input App")
    dmg = output / names.dmg
    manifest = output / names.manifest
    for path in (dmg, manifest):
        if path.exists() or path.is_symlink():
            raise PackagingError(f"refusing to overwrite existing artifact: {path.name}")
    return dmg, manifest


def render_manifest(
    *,
    version: str,
    git_sha: str | None,
    file_name: str,
    size_bytes: int,
    sha256: str,
    architectures: tuple[str, ...],
    minimum_macos: str,
    signing_state: str,
    notarization_state: str,
) -> str:
    payload = {
        "architectures": list(architectures),
        "file_name": file_name,
        "git_sha": git_sha,
        "minimum_macos": minimum_macos,
        "notarization": notarization_state,
        "sha256": sha256,
        "signing": signing_state,
        "size_bytes": size_bytes,
        "version": version,
    }
    return json.dumps(payload, indent=2, sort_keys=True) + "\n"


def _read_plist(app: Path) -> dict[str, object]:
    plist_path = app / "Contents" / "Info.plist"
    try:
        with plist_path.open("rb") as stream:
            value = plistlib.load(stream)
    except (OSError, plistlib.InvalidFileException) as error:
        raise PackagingError("input App has no valid Contents/Info.plist") from error
    if not isinstance(value, dict):
        raise PackagingError("input App Info.plist is not a dictionary")
    return value


def inspect_app(app: Path, *, lipo: Path, vtool: Path) -> AppFacts:
    if app.is_symlink() or not app.is_dir() or app.name != APP_NAME:
        raise PackagingError("--app must name a real LingTai.app directory, not a symlink")
    plist = _read_plist(app)
    if plist.get("CFBundleIdentifier") != BUNDLE_ID:
        raise PackagingError("App bundle identifier is not ai.lingtai.desktop")
    version = plist.get("CFBundleShortVersionString")
    build_version = plist.get("CFBundleVersion")
    minimum = plist.get("LSMinimumSystemVersion")
    if not isinstance(version, str) or VERSION_PATTERN.fullmatch(version) is None:
        raise PackagingError("App has an invalid CFBundleShortVersionString")
    if build_version != version:
        raise PackagingError("App bundle and short versions disagree")
    if minimum != MINIMUM_MACOS:
        raise PackagingError(f"App must declare LSMinimumSystemVersion {MINIMUM_MACOS}")

    executable_name = plist.get("CFBundleExecutable")
    if not isinstance(executable_name, str) or not executable_name:
        raise PackagingError("App has no CFBundleExecutable")
    executable = app / "Contents" / "MacOS" / executable_name
    if not executable.is_file():
        raise PackagingError("App main executable is absent")
    architectures = parse_architectures(
        run_checked([lipo, "-archs", executable], label="lipo architecture check")
    )
    if architectures != REQUIRED_ARCHITECTURES:
        raise PackagingError("App main executable must contain arm64 and x86_64 only")

    minimums = parse_minos(
        run_checked([vtool, "-show-build", executable], label="vtool minimum check")
    )
    if set(minimums) != set(REQUIRED_ARCHITECTURES):
        raise PackagingError("App main executable lacks a minimum for both architectures")
    if any(value != MINIMUM_MACOS for value in minimums.values()):
        raise PackagingError(f"App executable slices must target macOS {MINIMUM_MACOS}")
    return AppFacts(version, architectures, minimum)


def _input_snapshot(root: Path) -> tuple[tuple[str, str, int, int, str], ...]:
    entries: list[tuple[str, str, int, int, str]] = []
    for path in sorted([root, *root.rglob("*")], key=lambda item: os.fspath(item)):
        relative = os.fspath(path.relative_to(root)) if path != root else "."
        stat = path.lstat()
        if path.is_symlink():
            kind = "symlink"
            target = os.readlink(path)
        elif path.is_dir():
            kind = "directory"
            target = ""
        elif path.is_file():
            kind = "file"
            target = ""
        else:
            kind = "other"
            target = ""
        entries.append((relative, kind, stat.st_size, stat.st_mtime_ns, target))
    return tuple(entries)


def _normalize_mtimes(root: Path, epoch: int) -> None:
    for path in sorted([*root.rglob("*"), root], reverse=True):
        os.utime(path, (epoch, epoch), follow_symlinks=False)


def _git_sha(repository_root: Path) -> str | None:
    try:
        value = run_checked(
            ["git", "-C", repository_root, "rev-parse", "HEAD"],
            label="git revision check",
        ).strip()
    except PackagingError:
        return None
    return value if re.fullmatch(r"[0-9a-f]{40}", value) else None


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _validate_identity(identity: str, security: Path) -> None:
    output = run_checked(
        [security, "find-identity", "-v", "-p", "codesigning"],
        label="Developer ID identity check",
        sensitive=True,
    )
    if f'"{identity}"' not in output:
        raise PackagingError("requested Developer ID Application identity is unavailable")


def _validate_staged_signature(app: Path, codesign: Path, release: bool) -> None:
    run_checked(
        [codesign, "--verify", "--deep", "--strict", "--verbose=2", app],
        label="staged App code-signature verification",
    )
    details = run_checked(
        [codesign, "--display", "--verbose=4", app],
        label="staged App signature inspection",
    )
    if release:
        if "Authority=Developer ID Application:" not in details:
            raise PackagingError("release App is not signed by Developer ID Application")
        if "(runtime)" not in details or "Timestamp=" not in details:
            raise PackagingError("release App lacks hardened runtime or secure timestamp")
    elif "Signature=adhoc" not in details:
        raise PackagingError("diagnostic App is not unmistakably ad-hoc signed")


def _notary_environment(environment: Mapping[str, str]) -> dict[str, str]:
    allowed = {
        "HOME",
        "LANG",
        "LC_ALL",
        "LOGNAME",
        "PATH",
        "SECURITYSESSIONID",
        "TMPDIR",
        "USER",
        "__CF_USER_TEXT_ENCODING",
    }
    return {key: environment[key] for key in allowed if key in environment}


def package(arguments: argparse.Namespace, environment: Mapping[str, str]) -> tuple[Path, Path]:
    if sys.platform != "darwin":
        raise PackagingError("macOS packaging is supported only on macOS")
    credentials = resolve_release_credentials(
        arguments.mode,
        arguments.signing_identity,
        arguments.notary_profile,
        environment,
    )
    tools = {
        name: require_tool(name, environment)
        for name in ("codesign", "ditto", "hdiutil", "lipo", "xcrun")
    }
    if credentials:
        tools["security"] = require_tool("security", environment)
    vtool = Path(
        run_checked([tools["xcrun"], "--find", "vtool"], label="vtool lookup").strip()
    )
    notarytool: Path | None = None
    stapler: Path | None = None
    if credentials:
        notarytool = Path(
            run_checked(
                [tools["xcrun"], "--find", "notarytool"], label="notarytool lookup"
            ).strip()
        )
        stapler = Path(
            run_checked(
                [tools["xcrun"], "--find", "stapler"], label="stapler lookup"
            ).strip()
        )

    if arguments.app.is_symlink():
        raise PackagingError("--app must not be a symlink")
    app = arguments.app.resolve(strict=False)
    facts = inspect_app(app, lipo=tools["lipo"], vtool=vtool)
    qt_root = arguments.qt_root.resolve(strict=False)
    deploy_tool = qt_root / "bin" / "macdeployqt"
    if not deploy_tool.is_file() or not os.access(deploy_tool, os.X_OK):
        raise PackagingError("pinned Qt root has no executable bin/macdeployqt")
    names = artifact_names(facts.version)
    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    if arguments.output_dir.is_symlink() or not arguments.output_dir.is_dir():
        raise PackagingError("--output-dir must be a real directory, not a symlink")
    dmg_path, manifest_path = validate_output_paths(app, arguments.output_dir, names)
    before = _input_snapshot(app)

    if credentials:
        _validate_identity(credentials.identity, tools["security"])

    repository_root = Path(__file__).resolve().parents[1]
    epoch = arguments.source_date_epoch
    if epoch is None:
        try:
            epoch = int(
                run_checked(
                    ["git", "-C", repository_root, "show", "-s", "--format=%ct", "HEAD"],
                    label="git timestamp check",
                ).strip()
            )
        except (PackagingError, ValueError):
            epoch = 946684800

    with tempfile.TemporaryDirectory(
        prefix=".lingtai-macos-package-", dir=arguments.output_dir
    ) as temporary:
        scratch = Path(temporary)
        staged_app = scratch / APP_NAME
        run_checked(
            [tools["ditto"], "--rsrc", "--extattr", "--acl", app, staged_app],
            label="App staging copy",
        )
        deploy_arguments: list[str | os.PathLike[str]] = [
            deploy_tool,
            staged_app,
            "-verbose=2",
            "-always-overwrite",
        ]
        if credentials:
            deploy_arguments.extend(
                [f"-sign-for-notarization={credentials.identity}"]
            )
        else:
            deploy_arguments.append("-codesign=-")
        run_checked(
            deploy_arguments,
            label="Qt framework deployment",
            sensitive=credentials is not None,
        )
        inspect_app(staged_app, lipo=tools["lipo"], vtool=vtool)
        _validate_staged_signature(staged_app, tools["codesign"], credentials is not None)

        volume = scratch / "volume"
        volume.mkdir()
        run_checked(
            [tools["ditto"], "--rsrc", "--extattr", "--acl", staged_app, volume / APP_NAME],
            label="DMG volume App copy",
        )
        os.symlink("/Applications", volume / "Applications")
        _normalize_mtimes(volume, epoch)
        temporary_dmg = scratch / names.dmg
        run_checked(
            [
                tools["hdiutil"],
                "create",
                "-quiet",
                "-fs",
                "HFS+",
                "-format",
                "UDZO",
                "-imagekey",
                "zlib-level=9",
                "-volname",
                f"LingTai {facts.version}",
                "-srcfolder",
                volume,
                temporary_dmg,
            ],
            label="versioned DMG creation",
        )

        notarization_state = "not performed (diagnostic mode)"
        signing_state = "ad-hoc App; unsigned DMG (diagnostic only)"
        if credentials:
            run_checked(
                [
                    tools["codesign"],
                    "--force",
                    "--sign",
                    credentials.identity,
                    "--timestamp",
                    temporary_dmg,
                ],
                label="DMG Developer ID signing",
                sensitive=True,
            )
            assert notarytool is not None
            assert stapler is not None
            notary_output = run_checked(
                [
                    notarytool,
                    "submit",
                    temporary_dmg,
                    "--keychain-profile",
                    credentials.notary_profile,
                    "--wait",
                    "--output-format",
                    "json",
                ],
                label="Apple notarization",
                environment=_notary_environment(environment),
                sensitive=True,
            )
            try:
                notary_result = json.loads(notary_output)
            except json.JSONDecodeError as error:
                raise PackagingError("Apple notarization returned invalid status") from error
            if notary_result.get("status") != "Accepted":
                raise PackagingError("Apple notarization did not return Accepted")
            run_checked(
                [stapler, "staple", temporary_dmg],
                label="notarization ticket stapling",
                sensitive=True,
            )
            run_checked(
                [stapler, "validate", temporary_dmg],
                label="stapled-ticket validation",
                sensitive=True,
            )
            signing_state = "Developer ID Application; hardened runtime; timestamped"
            notarization_state = "Apple accepted; ticket stapled and validated"

        verification_arguments: list[str | os.PathLike[str]] = [
            sys.executable,
            repository_root / "scripts" / "verify-macos-package.py",
            "--dmg",
            temporary_dmg,
            "--expected-version",
            facts.version,
        ]
        if credentials:
            verification_arguments.append("--require-release-ready")
        run_checked(
            verification_arguments,
            label="independent final package verification",
        )
        if before != _input_snapshot(app):
            raise PackagingError("input App changed during packaging; artifact is untrusted")
        shutil.move(os.fspath(temporary_dmg), dmg_path)

    manifest = render_manifest(
        version=facts.version,
        git_sha=_git_sha(repository_root),
        file_name=dmg_path.name,
        size_bytes=dmg_path.stat().st_size,
        sha256=_sha256(dmg_path),
        architectures=facts.architectures,
        minimum_macos=facts.minimum_macos,
        signing_state=signing_state,
        notarization_state=notarization_state,
    )
    try:
        with manifest_path.open("x", encoding="utf-8") as stream:
            stream.write(manifest)
    except FileExistsError as error:
        raise PackagingError("manifest appeared during packaging; refusing overwrite") from error
    return dmg_path, manifest_path


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Stage a self-contained LingTai.app and create a versioned macOS DMG."
    )
    parser.add_argument("--mode", required=True, choices=("diagnostic", "release"))
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument(
        "--qt-root",
        type=Path,
        default=Path(os.environ.get("QT_ROOT", Path.home() / "Qt/6.11.1/macos")),
    )
    parser.add_argument("--signing-identity")
    parser.add_argument("--notary-profile")
    parser.add_argument("--source-date-epoch", type=int)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        arguments = build_parser().parse_args(argv)
        dmg, manifest = package(arguments, os.environ)
    except PackagingError as error:
        print(f"package-macos: {error}", file=sys.stderr)
        return 1
    print(f"DMG: {dmg}")
    print(f"manifest: {manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
