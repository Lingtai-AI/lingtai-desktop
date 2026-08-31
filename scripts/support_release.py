#!/usr/bin/env python3
"""Deterministically produce or validate the exact Desktop support release asset set."""

from __future__ import annotations

import argparse
import os
import shutil
import stat
import sys
from pathlib import Path

try:
    from scripts import desktop_user_cli as cli
except ImportError:  # Direct execution from the repository's scripts directory.
    import desktop_user_cli as cli  # type: ignore


SOURCE_NAMES = {
    cli.SUPPORT_PAYLOAD_NAMES[0]: "desktop_user_cli.py",
    cli.SUPPORT_PAYLOAD_NAMES[1]: "verify-app-archive.py",
}


def _source_payloads(repository_root: Path) -> dict[str, bytes]:
    scripts = repository_root / "scripts"
    payloads: dict[str, bytes] = {}
    for asset_name, source_name in SOURCE_NAMES.items():
        source = scripts / source_name
        facts = cli._regular_nofollow(source, f"support release source {source_name}")
        if (facts.st_uid != os.geteuid() or facts.st_nlink != 1
                or facts.st_size <= 0 or facts.st_size > cli.MAX_SUPPORT_PAYLOAD_BYTES):
            raise cli.DesktopCLIError(
                f"support release source {source_name} ownership or size is invalid"
            )
        content = cli._read_bytes_nofollow(
            source, f"support release source {source_name}",
            cli.MAX_SUPPORT_PAYLOAD_BYTES,
        )
        try:
            compile(content, source_name, "exec", dont_inherit=True)
        except (SyntaxError, ValueError, TypeError) as error:
            raise cli.DesktopCLIError(
                f"support release source {source_name} is not valid Python"
            ) from error
        payloads[asset_name] = content
    return payloads


def _write_asset(path: Path, content: bytes) -> None:
    descriptor: int | None = None
    try:
        descriptor = os.open(
            path, os.O_WRONLY | os.O_CREAT | os.O_EXCL
            | getattr(os, "O_NOFOLLOW", 0), 0o600,
        )
        with os.fdopen(descriptor, "wb") as stream:
            descriptor = None
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(path, 0o600, follow_symlinks=False)
    except FileExistsError as error:
        raise cli.DesktopCLIError(f"refusing to replace support release asset {path.name}") from error
    except OSError as error:
        raise cli.DesktopCLIError(f"could not write support release asset {path.name}") from error
    finally:
        if descriptor is not None:
            os.close(descriptor)


def validate_support_release(output: Path) -> cli.SupportManifest:
    output = Path(output)
    try:
        directory = output.lstat()
    except OSError as error:
        raise cli.DesktopCLIError("support release directory is missing") from error
    if (stat.S_ISLNK(directory.st_mode) or not stat.S_ISDIR(directory.st_mode)
            or directory.st_uid != os.geteuid()
            or stat.S_IMODE(directory.st_mode) != 0o700):
        raise cli.DesktopCLIError("support release directory ownership or mode is invalid")
    expected = {cli.SUPPORT_MANIFEST_NAME, *cli.SUPPORT_PAYLOAD_NAMES}
    try:
        actual = {entry.name for entry in os.scandir(output)}
    except OSError as error:
        raise cli.DesktopCLIError("support release directory could not be enumerated") from error
    if actual != expected:
        raise cli.DesktopCLIError("support release file set is not exact")
    manifest_bytes, _ = cli._read_managed_support_file(
        output / cli.SUPPORT_MANIFEST_NAME, "support release manifest",
        cli.MAX_SUPPORT_MANIFEST_BYTES,
    )
    manifest = cli.parse_support_manifest(manifest_bytes)
    payloads: dict[str, bytes] = {}
    for declared in manifest.files:
        payloads[declared.name], _ = cli._read_managed_support_file(
            output / declared.name, f"support release payload {declared.name}",
            cli.MAX_SUPPORT_PAYLOAD_BYTES, expected_size=declared.size,
        )
    verified = cli._verified_support_payloads(manifest_bytes, payloads)
    if verified != manifest:
        raise cli.DesktopCLIError("support release changed during validation")
    return verified


def produce_support_release(
        output: Path, version: str, *, repository_root: Path | None = None,
) -> cli.SupportManifest:
    if not cli._is_safe_version(version):
        raise cli.DesktopCLIError("support release version must be a safe x.y.z value")
    root = (Path(repository_root) if repository_root is not None
            else Path(__file__).resolve().parents[1])
    payloads = _source_payloads(root)
    manifest_bytes = cli.build_support_manifest_bytes(version, f"v{version}", payloads)
    output = Path(output)
    created = False
    identity: tuple[int, int] | None = None
    try:
        output.mkdir(mode=0o700, parents=False, exist_ok=False)
        created = True
        os.chmod(output, 0o700, follow_symlinks=False)
        identity = cli._identity(output)
        _write_asset(output / cli.SUPPORT_MANIFEST_NAME, manifest_bytes)
        for name in cli.SUPPORT_PAYLOAD_NAMES:
            _write_asset(output / name, payloads[name])
        cli._fsync_directory(output)
        cli._fsync_directory(output.parent)
        return validate_support_release(output)
    except FileExistsError as error:
        raise cli.DesktopCLIError("support release output already exists; refusing to clobber") from error
    except Exception:
        if (created and identity is not None and cli._matches_identity(output, identity)
                and not output.is_symlink() and output.is_dir()):
            shutil.rmtree(output)
            try:
                cli._fsync_directory(output.parent)
            except cli.DesktopCLIError:
                pass
        raise


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description="Build or validate exact offline LingTai Desktop support release assets."
    )
    subcommands = result.add_subparsers(dest="command", required=True)
    build = subcommands.add_parser("build")
    build.add_argument("--version", required=True)
    build.add_argument("--output", type=Path, required=True)
    build.add_argument("--repository-root", type=Path)
    validate = subcommands.add_parser("validate")
    validate.add_argument("output", type=Path)
    return result


def main(argv: list[str] | None = None) -> int:
    values = parser().parse_args(argv)
    try:
        if values.command == "build":
            manifest = produce_support_release(
                values.output, values.version, repository_root=values.repository_root,
            )
        else:
            manifest = validate_support_release(values.output)
    except cli.DesktopCLIError as error:
        print(f"support-release: {error}", file=sys.stderr)
        return 1
    print(f"support release valid: {manifest.generation_id}")
    print(f"manifest sha256: {manifest.manifest_sha256}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
