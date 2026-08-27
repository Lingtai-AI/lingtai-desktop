#!/usr/bin/env python3
"""Independently verify and smoke a LingTai Desktop macOS DMG."""

from __future__ import annotations

import argparse
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
SYSTEM_PREFIXES = ("/System/Library/", "/usr/lib/")
SMOKE_MARKERS = (
    "LINGTAI_NATIVE_SHELL_READY",
    "LINGTAI_LIB_UI_FULL_TARGET_SMOKE_OK",
)


class VerificationError(RuntimeError):
    """A bounded package-verification failure."""


def run_checked(
    arguments: Sequence[str | os.PathLike[str]],
    *,
    label: str,
    environment: Mapping[str, str] | None = None,
    timeout: int | None = None,
) -> str:
    try:
        result = subprocess.run(
            [os.fspath(argument) for argument in arguments],
            env=dict(environment) if environment is not None else None,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise VerificationError(f"{label} timed out") from error
    except OSError as error:
        raise VerificationError(f"{label} could not start") from error
    if result.returncode != 0:
        output = (result.stdout or "").strip().replace("\0", "")[-1200:]
        suffix = f": {output}" if output else ""
        raise VerificationError(
            f"{label} failed (exit {result.returncode}){suffix}"
        )
    return result.stdout or ""


def require_tool(name: str) -> Path:
    resolved = shutil.which(name, path=os.environ.get("PATH"))
    if not resolved:
        raise VerificationError(f"required tool is unavailable: {name}")
    return Path(resolved)


def _parse_architectures(output: str) -> tuple[str, ...]:
    return tuple(sorted(set(output.split())))


def _parse_minos(output: str) -> dict[str, str]:
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
    return facts


def _read_plist(app: Path) -> dict[str, object]:
    try:
        with (app / "Contents" / "Info.plist").open("rb") as stream:
            value = plistlib.load(stream)
    except (OSError, plistlib.InvalidFileException) as error:
        raise VerificationError("packaged App has no valid Info.plist") from error
    if not isinstance(value, dict):
        raise VerificationError("packaged App Info.plist is not a dictionary")
    return value


def _is_inside(candidate: Path, app: Path) -> bool:
    try:
        candidate.resolve(strict=True).relative_to(app.resolve(strict=True))
    except (FileNotFoundError, ValueError):
        return False
    return True


def _macho_files(app: Path, file_tool: Path) -> list[Path]:
    files: list[Path] = []
    for candidate in sorted(app.rglob("*"), key=lambda path: os.fspath(path)):
        if candidate.is_symlink() or not candidate.is_file():
            continue
        description = run_checked(
            [file_tool, "-b", candidate], label="Mach-O file classification"
        )
        if "Mach-O" in description:
            files.append(candidate)
    if not files:
        raise VerificationError("packaged App contains no Mach-O binaries")
    return files


def _rpaths(binary: Path, otool: Path) -> list[str]:
    output = run_checked([otool, "-l", binary], label="Mach-O rpath inspection")
    result: list[str] = []
    awaiting_path = False
    for line in output.splitlines():
        stripped = line.strip()
        if stripped == "cmd LC_RPATH":
            awaiting_path = True
        elif awaiting_path and stripped.startswith("path "):
            result.append(stripped[5:].rsplit(" (offset ", 1)[0])
            awaiting_path = False
    return result


def _install_id(binary: Path, otool: Path) -> str | None:
    output = run_checked([otool, "-D", binary], label="Mach-O install-id inspection")
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    return lines[1] if len(lines) > 1 else None


def _dependencies(binary: Path, otool: Path) -> list[str]:
    output = run_checked([otool, "-L", binary], label="Mach-O dependency inspection")
    dependencies: list[str] = []
    for line in output.splitlines()[1:]:
        stripped = line.strip()
        if not stripped:
            continue
        dependencies.append(stripped.split(" (compatibility version", 1)[0])
    install_id = _install_id(binary, otool)
    return [dependency for dependency in dependencies if dependency != install_id]


def _expand_anchor(value: str, binary: Path, executable_directory: Path) -> Path | None:
    if value == "@loader_path":
        return binary.parent
    if value.startswith("@loader_path/"):
        return binary.parent / value.removeprefix("@loader_path/")
    if value == "@executable_path":
        return executable_directory
    if value.startswith("@executable_path/"):
        return executable_directory / value.removeprefix("@executable_path/")
    if value.startswith("/"):
        return Path(value)
    return None


def _verify_binary_links(
    binary: Path,
    app: Path,
    executable_directory: Path,
    executable_rpaths: Sequence[str],
    otool: Path,
) -> None:
    rpaths = _rpaths(binary, otool)
    for rpath in rpaths:
        if rpath.startswith("/") and not rpath.startswith(SYSTEM_PREFIXES):
            raise VerificationError(
                f"absolute non-system rpath in {binary.relative_to(app)}"
            )
        expanded = _expand_anchor(rpath, binary, executable_directory)
        if expanded is None and not rpath.startswith(SYSTEM_PREFIXES):
            raise VerificationError(f"unsupported rpath in {binary.relative_to(app)}")

    for dependency in _dependencies(binary, otool):
        if dependency.startswith(SYSTEM_PREFIXES):
            continue
        if dependency.startswith("/"):
            raise VerificationError(
                f"absolute non-system dependency in {binary.relative_to(app)}"
            )
        candidates: list[Path] = []
        if dependency.startswith("@rpath/"):
            suffix = dependency.removeprefix("@rpath/")
            # dyld walks the loader chain's run paths. Qt plugins retain a
            # relative SDK rpath, while the main executable contributes the
            # deployed Contents/Frameworks rpath that actually resolves them.
            for rpath in [*rpaths, *executable_rpaths]:
                root = _expand_anchor(rpath, binary, executable_directory)
                if root is not None:
                    candidates.append(root / suffix)
        else:
            expanded = _expand_anchor(dependency, binary, executable_directory)
            if expanded is not None:
                candidates.append(expanded)
        if not any(_is_inside(candidate, app) for candidate in candidates):
            raise VerificationError(
                f"unresolved bundled dependency in {binary.relative_to(app)}"
            )


def verify_app(
    app: Path,
    expected_version: str,
    *,
    require_release_ready: bool,
    tools: Mapping[str, Path],
    vtool: Path,
) -> Path:
    plist = _read_plist(app)
    expected = {
        "CFBundleIdentifier": BUNDLE_ID,
        "CFBundleShortVersionString": expected_version,
        "CFBundleVersion": expected_version,
        "LSMinimumSystemVersion": MINIMUM_MACOS,
    }
    for key, value in expected.items():
        if plist.get(key) != value:
            raise VerificationError(f"packaged App has incorrect {key}")
    executable_name = plist.get("CFBundleExecutable")
    if not isinstance(executable_name, str):
        raise VerificationError("packaged App has no CFBundleExecutable")
    executable = app / "Contents" / "MacOS" / executable_name
    if not executable.is_file():
        raise VerificationError("packaged App main executable is absent")
    architectures = _parse_architectures(
        run_checked([tools["lipo"], "-archs", executable], label="main architecture check")
    )
    if architectures != REQUIRED_ARCHITECTURES:
        raise VerificationError("packaged App is not arm64+x86_64 universal")

    macho_files = _macho_files(app, tools["file"])
    executable_rpaths = _rpaths(executable, tools["otool"])
    for binary in macho_files:
        binary_architectures = _parse_architectures(
            run_checked(
                [tools["lipo"], "-archs", binary], label="bundled architecture check"
            )
        )
        if binary_architectures != REQUIRED_ARCHITECTURES:
            raise VerificationError(
                f"non-universal bundled binary: {binary.relative_to(app)}"
            )
        minimums = _parse_minos(
            run_checked([vtool, "-show-build", binary], label="bundled minimum check")
        )
        if set(minimums) != set(REQUIRED_ARCHITECTURES) or any(
            value != MINIMUM_MACOS for value in minimums.values()
        ):
            raise VerificationError(
                f"incorrect bundled minimum macOS in {binary.relative_to(app)}"
            )
        _verify_binary_links(
            binary,
            app,
            executable.parent,
            executable_rpaths,
            tools["otool"],
        )

    run_checked(
        [tools["codesign"], "--verify", "--deep", "--strict", "--verbose=2", app],
        label="App code-signature verification",
    )
    signature = run_checked(
        [tools["codesign"], "--display", "--verbose=4", app],
        label="App signature inspection",
    )
    if require_release_ready:
        if "Authority=Developer ID Application:" not in signature:
            raise VerificationError("release App lacks Developer ID Application signing")
        if "(runtime)" not in signature or "Timestamp=" not in signature:
            raise VerificationError("release App lacks hardened runtime or timestamp")
        run_checked(
            [tools["spctl"], "--assess", "--type", "execute", "--verbose=2", app],
            label="Gatekeeper App assessment",
        )
    elif "Signature=adhoc" not in signature:
        raise VerificationError("diagnostic App must be ad-hoc signed")
    return executable


def _verify_read_only_mount(mountpoint: Path, mount_tool: Path) -> None:
    output = run_checked([mount_tool], label="mounted-volume inspection")
    matching = [line for line in output.splitlines() if os.fspath(mountpoint) in line]
    if not matching or not any("read-only" in line for line in matching):
        raise VerificationError("DMG did not mount read-only")


def _minimal_smoke_environment(fake_home: Path, fake_tmp: Path) -> dict[str, str]:
    return {
        "HOME": os.fspath(fake_home),
        "TMPDIR": os.fspath(fake_tmp),
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
        "LANG": "en_US.UTF-8",
        "LC_ALL": "en_US.UTF-8",
    }


def verify_package(arguments: argparse.Namespace) -> None:
    if sys.platform != "darwin":
        raise VerificationError("macOS package verification is supported only on macOS")
    expected_name = f"LingTai-{arguments.expected_version}-macOS-universal.dmg"
    dmg = arguments.dmg.resolve(strict=True)
    if dmg.name != expected_name or not dmg.is_file() or dmg.is_symlink():
        raise VerificationError("DMG does not have the deterministic versioned file name")

    tools = {
        name: require_tool(name)
        for name in (
            "codesign",
            "ditto",
            "file",
            "hdiutil",
            "lipo",
            "mount",
            "otool",
            "spctl",
            "xcrun",
        )
    }
    vtool = Path(
        run_checked([tools["xcrun"], "--find", "vtool"], label="vtool lookup").strip()
    )
    stapler: Path | None = None
    if arguments.require_release_ready:
        stapler = Path(
            run_checked(
                [tools["xcrun"], "--find", "stapler"], label="stapler lookup"
            ).strip()
        )

    with tempfile.TemporaryDirectory(prefix="lingtai-macos-verify-") as temporary:
        scratch = Path(temporary)
        mountpoint = scratch / "mounted"
        mountpoint.mkdir()
        relocated = scratch / "relocated" / APP_NAME
        relocated.parent.mkdir()
        attached = False
        try:
            run_checked(
                [
                    tools["hdiutil"],
                    "attach",
                    "-readonly",
                    "-nobrowse",
                    "-noautoopen",
                    "-mountpoint",
                    mountpoint,
                    "-plist",
                    dmg,
                ],
                label="read-only DMG mount",
            )
            attached = True
            _verify_read_only_mount(mountpoint, tools["mount"])
            packaged_app = mountpoint / APP_NAME
            applications = mountpoint / "Applications"
            if not packaged_app.is_dir() or packaged_app.is_symlink():
                raise VerificationError("DMG does not contain LingTai.app")
            if not applications.is_symlink() or os.readlink(applications) != "/Applications":
                raise VerificationError("DMG Applications item is not a /Applications symlink")
            visible_items = sorted(
                path.name for path in mountpoint.iterdir() if not path.name.startswith(".")
            )
            if visible_items != ["Applications", APP_NAME]:
                raise VerificationError("DMG has unexpected visible top-level items")
            app_bundles = sorted(path.name for path in mountpoint.glob("*.app"))
            if app_bundles != [APP_NAME]:
                raise VerificationError("DMG contains an unexpected App bundle layout")
            run_checked(
                [tools["ditto"], "--rsrc", "--extattr", "--acl", packaged_app, relocated],
                label="relocated App copy",
            )
        finally:
            if attached:
                run_checked(
                    [tools["hdiutil"], "detach", mountpoint],
                    label="DMG detach",
                )

        executable = verify_app(
            relocated,
            arguments.expected_version,
            require_release_ready=arguments.require_release_ready,
            tools=tools,
            vtool=vtool,
        )
        fake_home = scratch / "home"
        fake_tmp = scratch / "tmp"
        fake_home.mkdir()
        fake_tmp.mkdir()
        smoke_output = run_checked(
            [executable, "--smoke"],
            label="relocated packaged-App smoke",
            environment=_minimal_smoke_environment(fake_home, fake_tmp),
            timeout=arguments.smoke_timeout,
        )
        positions = [smoke_output.find(marker) for marker in SMOKE_MARKERS]
        if any(position < 0 for position in positions) or positions != sorted(positions):
            raise VerificationError("relocated smoke markers are absent or out of order")

    if arguments.require_release_ready:
        assert stapler is not None
        run_checked(
            [tools["codesign"], "--verify", "--strict", "--verbose=2", dmg],
            label="DMG code-signature verification",
        )
        run_checked([stapler, "validate", dmg], label="DMG stapled-ticket validation")
        run_checked(
            [
                tools["spctl"],
                "--assess",
                "--type",
                "open",
                "--context",
                "context:primary-signature",
                "--verbose=2",
                dmg,
            ],
            label="Gatekeeper DMG assessment",
        )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Mount, relocate, inspect, and smoke a LingTai Desktop macOS DMG."
    )
    parser.add_argument("--dmg", required=True, type=Path)
    parser.add_argument("--expected-version", required=True)
    parser.add_argument("--require-release-ready", action="store_true")
    parser.add_argument("--smoke-timeout", type=int, default=15)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        verify_package(build_parser().parse_args(argv))
    except (VerificationError, FileNotFoundError) as error:
        print(f"verify-macos-package: {error}", file=sys.stderr)
        return 1
    print("verify-macos-package: package layout, linkage, signing mode, and smoke passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
