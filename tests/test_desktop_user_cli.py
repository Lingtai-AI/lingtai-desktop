#!/usr/bin/env python3
"""Hermetic contracts for the user-level LingTai Desktop installer/launcher."""

from __future__ import annotations

import dataclasses
import hashlib
import io
import json
import os
import plistlib
import shutil
import stat
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from scripts import desktop_user_cli as cli


def write_artifacts(root: Path, version: str = "0.1.5") -> tuple[Path, Path]:
    archive = root / f"LingTai-{version}-macOS-universal.app.tar.gz"
    archive.write_bytes(b"fixed test App archive")
    with tempfile.TemporaryDirectory(dir=root) as temporary:
        app = Path(temporary) / "LingTai.app"
        make_app(app, version)
        executable = app / "Contents/MacOS/LingTai"
        executable_size = executable.stat().st_size
        executable_sha = hashlib.sha256(executable.read_bytes()).hexdigest()
        bundle_digest = cli.bundle_tree_digest(app)
    manifest = root / f"LingTai-{version}-macOS-universal.app.manifest.json"
    manifest.write_text(json.dumps({
        "architectures": ["arm64", "x86_64"],
        "archive_file_name": archive.name,
        "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
        "archive_size_bytes": archive.stat().st_size,
        "artifact_kind": "lingtai-portable-app-archive",
        "bundle_executable": "Contents/MacOS/LingTai",
        "bundle_identifier": "ai.lingtai.desktop",
        "bundle_name": "LingTai.app",
        "bundle_tree_sha256": bundle_digest,
        "bundle_version": version,
        "executable_sha256": executable_sha,
        "executable_size_bytes": executable_size,
        "minimum_macos": "13.0",
        "packaging_git_dirty": False,
        "packaging_git_head": "a" * 40,
        "packaging_git_tree": "b" * 40,
        "schema_version": 1,
        "version": version,
    }, sort_keys=True), encoding="utf-8")
    return archive, manifest


def make_app(path: Path, version: str) -> None:
    executable = path / "Contents" / "MacOS" / "LingTai"
    executable.parent.mkdir(parents=True, exist_ok=True)
    executable.write_bytes(b"#!/bin/sh\nexit 0\n")
    executable.chmod(0o755)
    with (path / "Contents" / "Info.plist").open("wb") as stream:
        plistlib.dump({
            "CFBundleIdentifier": "ai.lingtai.desktop",
            "CFBundleShortVersionString": version,
            "CFBundleVersion": version,
            "CFBundleExecutable": "LingTai",
            "LSMinimumSystemVersion": "13.0",
        }, stream)


def tree_snapshot(root: Path) -> tuple[tuple[str, str, int, bytes], ...]:
    result: list[tuple[str, str, int, bytes]] = []
    if not root.exists() and not root.is_symlink():
        return tuple()
    for path in sorted([root, *root.rglob("*")], key=lambda value: os.fspath(value)):
        relative = "." if path == root else os.fspath(path.relative_to(root))
        facts = path.lstat()
        mode = stat.S_IMODE(facts.st_mode)
        if path.is_symlink():
            kind, content = "symlink", os.fsencode(os.readlink(path))
        elif path.is_dir():
            kind, content = "directory", b""
        elif path.is_file():
            kind, content = "file", path.read_bytes()
        else:
            kind, content = "other", b""
        result.append((relative, kind, mode, content))
    return tuple(result)


class FakePlatform(cli.Platform):
    def __init__(self) -> None:
        self.calls: list[list[str]] = []
        self.exec_calls: list[list[str]] = []
        self.fail: str | None = None

    def verify_archive(self, repository_root: Path, archive: Path, manifest: Path) -> None:
        self.calls.append(["verify", str(archive), str(manifest)])
        if self.fail == "verifier":
            raise cli.DesktopCLIError("injected verifier failure")

    def verify_and_extract_archive(self, repository_root: Path, archive: Path,
                                   manifest: Path, destination: Path) -> Path:
        if self.fail == "extract":
            raise cli.DesktopCLIError("injected archive extraction failure")
        source = destination / "LingTai.app"
        make_app(source, cli.VERSION_PATTERN.search(archive.name).group(0))
        return source

    def smoke(self, executable: Path, fake_home: Path, fake_tmp: Path) -> None:
        self.calls.append([str(executable), "--smoke", str(fake_home), str(fake_tmp)])

    def open_app(self, app: Path) -> None:
        self.calls.append(["/usr/bin/open", str(app)])

    def exec_app(self, executable: Path, arguments: list[str]) -> None:
        self.exec_calls.append([str(executable), *arguments])


class FakeHTTPResponse:
    def __init__(self, status: int, headers: dict[str, str], body: bytes) -> None:
        self.status = status
        self.headers = headers
        self._stream = io.BytesIO(body)

    def read(self, size: int = -1) -> bytes:
        return self._stream.read(size)

    def close(self) -> None:
        self._stream.close()


class FakeReleaseTransport:
    def __init__(self, routes: dict[str, tuple[int, dict[str, str], bytes]]) -> None:
        self.routes = routes
        self.calls: list[tuple[str, dict[str, str], float]] = []

    def open(self, url: str, headers: dict[str, str], timeout: float) -> FakeHTTPResponse:
        self.calls.append((url, dict(headers), timeout))
        try:
            status, response_headers, body = self.routes[url]
        except KeyError as error:
            raise AssertionError(f"unexpected offline transport URL: {url}") from error
        return FakeHTTPResponse(status, dict(response_headers), body)


def official_release_transport(archive: Path, manifest: Path, version: str) -> FakeReleaseTransport:
    archive_name = archive.name
    manifest_name = manifest.name
    archive_url = f"https://github.com/Lingtai-AI/lingtai-desktop/releases/download/v{version}/{archive_name}"
    manifest_url = f"https://github.com/Lingtai-AI/lingtai-desktop/releases/download/v{version}/{manifest_name}"
    archive_final = f"https://release-assets.githubusercontent.com/github-production-release-asset/archive?token=private-archive"
    manifest_final = f"https://release-assets.githubusercontent.com/github-production-release-asset/manifest?token=private-manifest"
    metadata = json.dumps({
        "tag_name": f"v{version}",
        "draft": False,
        "prerelease": False,
        "assets": [
            {"name": archive_name, "browser_download_url": archive_url, "size": archive.stat().st_size},
            {"name": manifest_name, "browser_download_url": manifest_url, "size": manifest.stat().st_size},
        ],
    }).encode()
    return FakeReleaseTransport({
        "https://api.github.com/repos/Lingtai-AI/lingtai-desktop/releases/latest":
            (200, {"Content-Length": str(len(metadata))}, metadata),
        f"https://api.github.com/repos/Lingtai-AI/lingtai-desktop/releases/tags/v{version}":
            (200, {"Content-Length": str(len(metadata))}, metadata),
        archive_url: (302, {"Location": archive_final}, b""),
        manifest_url: (302, {"Location": manifest_final}, b""),
        archive_final: (200, {"Content-Length": str(archive.stat().st_size)}, archive.read_bytes()),
        manifest_final: (200, {"Content-Length": str(manifest.stat().st_size)}, manifest.read_bytes()),
    })


def unavailable_release_transport() -> FakeReleaseTransport:
    return FakeReleaseTransport({
        cli.OFFICIAL_LATEST_RELEASE_URL: (429, {}, b"offline test boundary"),
    })


class DesktopUserCLIContractTest(unittest.TestCase):
    def test_staged_app_smoke_uses_sixty_second_isolated_fail_closed_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "LingTai.app/Contents/MacOS/LingTai"
            fake_home = root / "home"
            fake_tmp = root / "tmp"
            successful = mock.Mock(
                returncode=0,
                stdout=(
                    "LINGTAI_NATIVE_SHELL_READY\n"
                    "LINGTAI_LIB_UI_FULL_TARGET_SMOKE_OK\n"
                ),
            )
            with mock.patch.object(cli.subprocess, "run", return_value=successful) as run:
                cli.Platform().smoke(executable, fake_home, fake_tmp)
            self.assertEqual(cli.STAGED_APP_SMOKE_TIMEOUT, 60)
            run.assert_called_once_with(
                [str(executable), "--smoke"],
                env={
                    "HOME": str(fake_home), "TMPDIR": str(fake_tmp),
                    "PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "LANG": "en_US.UTF-8",
                    "LC_ALL": "en_US.UTF-8",
                },
                stdout=cli.subprocess.PIPE, stderr=cli.subprocess.STDOUT,
                text=True, timeout=60, check=False,
            )

            for output in (
                "LINGTAI_NATIVE_SHELL_READY\n",
                "LINGTAI_LIB_UI_FULL_TARGET_SMOKE_OK\nLINGTAI_NATIVE_SHELL_READY\n",
            ):
                with self.subTest(output=output), mock.patch.object(
                    cli.subprocess, "run",
                    return_value=mock.Mock(returncode=0, stdout=output),
                ):
                    with self.assertRaisesRegex(
                        cli.DesktopCLIError, "markers are absent or out of order",
                    ):
                        cli.Platform().smoke(executable, fake_home, fake_tmp)

    def test_official_url_rejects_unicode_before_production_transport(self) -> None:
        connection = mock.Mock()
        connection.request.side_effect = UnicodeEncodeError(
            "ascii", "\N{SNOWMAN}", 0, 1, "ordinal not in range(128)",
        )
        with mock.patch.object(
                cli.http.client, "HTTPSConnection", return_value=connection,
        ) as connection_constructor:
            with self.assertRaises(cli.DesktopCLIError):
                cli._open_official_response(
                    "https://release-assets.githubusercontent.com/\N{SNOWMAN}",
                    cli.ReleaseTransport(), 1.0,
                )
        connection_constructor.assert_not_called()

        encoded = cli._validate_official_url(
            "https://release-assets.githubusercontent.com/%E2%98%83",
            "official release",
        )
        self.assertEqual(encoded.path, "/%E2%98%83")

    def test_official_latest_and_exact_bootstrap_download_verified_release(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifacts = root / "release"
            artifacts.mkdir()
            archive, manifest = write_artifacts(artifacts, "0.1.6")

            latest_home = root / "latest-home"
            latest_home.mkdir()
            latest_platform = FakePlatform()
            latest_transport = official_release_transport(archive, manifest, "0.1.6")
            latest_output: list[str] = []
            self.assertEqual(cli.bootstrap_main(
                [], home=latest_home, platform=latest_platform,
                transport=latest_transport, effective_uid=501,
                output=latest_output.append,
            ), 0)
            self.assertEqual(
                os.readlink(latest_home / ".local/share/lingtai-desktop/current"),
                "versions/0.1.6",
            )
            self.assertEqual(latest_transport.calls[0][0], cli.OFFICIAL_LATEST_RELEASE_URL)

            exact_home = root / "exact-home"
            exact_home.mkdir()
            exact_platform = FakePlatform()
            exact_transport = official_release_transport(archive, manifest, "0.1.6")
            self.assertEqual(cli.bootstrap_main(
                ["--version", "0.1.6"], home=exact_home, platform=exact_platform,
                transport=exact_transport, effective_uid=501,
                output=lambda _: None,
            ), 0)
            self.assertEqual(
                exact_transport.calls[0][0],
                "https://api.github.com/repos/Lingtai-AI/lingtai-desktop/releases/tags/v0.1.6",
            )
            self.assertTrue(all(call[1]["User-Agent"].startswith("lingtai-desktop/")
                                and call[1]["Accept"] for call in latest_transport.calls))
            verify_calls = [call for call in latest_platform.calls + exact_platform.calls
                            if call[0] == "verify"]
            self.assertEqual(len(verify_calls), 2)
            self.assertTrue(all(Path(call[1]).name == archive.name
                                and Path(call[2]).name == manifest.name for call in verify_calls))
            self.assertTrue(all(not Path(call[1]).parent.exists() for call in verify_calls))

            refused_home = root / "refused-home"
            refused_home.mkdir()
            refused_before = tree_snapshot(refused_home)
            with mock.patch("sys.stderr", new=io.StringIO()):
                self.assertEqual(cli.bootstrap_main([
                    "--version", "0.1.6", "--archive", str(archive),
                    "--manifest", str(manifest),
                ], home=refused_home, platform=FakePlatform(),
                    transport=FakeReleaseTransport({}), effective_uid=501,
                    output=lambda _: None), 1)
                self.assertEqual(cli.bootstrap_main([
                    "--archive", str(archive),
                ], home=refused_home, platform=FakePlatform(),
                    transport=FakeReleaseTransport({}), effective_uid=501,
                    output=lambda _: None), 1)
            self.assertEqual(tree_snapshot(refused_home), refused_before)

    def test_explicit_update_forces_official_install_and_preserves_local_pair_mode(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            release_root = root / "release"
            release_root.mkdir()
            old_archive, old_manifest = write_artifacts(root, "0.1.5")
            new_archive, new_manifest = write_artifacts(release_root, "0.1.6")

            home = root / "home"
            home.mkdir()
            platform = FakePlatform()
            cli.install(old_archive, old_manifest, home=home,
                        platform=platform, effective_uid=501)
            transport = official_release_transport(new_archive, new_manifest, "0.1.6")
            output: list[str] = []
            self.assertEqual(cli.run_installed(
                ["update"], home=home, platform=platform, output=output.append,
                transport=transport, effective_uid=501,
            ), 0)
            managed = home / ".local/share/lingtai-desktop"
            self.assertEqual(os.readlink(managed / "current"), "versions/0.1.6")
            self.assertEqual(transport.calls[0][0], cli.OFFICIAL_LATEST_RELEASE_URL)
            self.assertIn("updated LingTai Desktop to 0.1.6", output)

            transaction_before = (
                os.readlink(managed / "current"),
                tree_snapshot(managed / "versions/0.1.6"),
                (managed / "receipts/0.1.6.json").read_bytes(),
            )
            verify_count = [call[0] for call in platform.calls].count("verify")
            same_transport = official_release_transport(new_archive, new_manifest, "0.1.6")
            cli.run_installed(
                ["update"], home=home, platform=platform,
                transport=same_transport, effective_uid=501,
                clock=lambda: 1_500.0, output=lambda _: None,
            )
            self.assertEqual((
                os.readlink(managed / "current"),
                tree_snapshot(managed / "versions/0.1.6"),
                (managed / "receipts/0.1.6.json").read_bytes(),
            ), transaction_before)
            self.assertEqual([call[0] for call in platform.calls].count("verify"),
                             verify_count + 1)
            same_verify = [call for call in platform.calls if call[0] == "verify"][-1]
            self.assertFalse(Path(same_verify[1]).parent.exists())

            exact_home = root / "exact-home"
            exact_home.mkdir()
            exact_platform = FakePlatform()
            cli.install(old_archive, old_manifest, home=exact_home,
                        platform=exact_platform, effective_uid=501)
            exact_transport = official_release_transport(new_archive, new_manifest, "0.1.6")
            cli.run_installed(
                ["update", "--version", "0.1.6"], home=exact_home,
                platform=exact_platform, transport=exact_transport,
                effective_uid=501,
            )
            self.assertEqual(
                exact_transport.calls[0][0],
                "https://api.github.com/repos/Lingtai-AI/lingtai-desktop/releases/tags/v0.1.6",
            )

            local_home = root / "local-home"
            local_home.mkdir()
            local_platform = FakePlatform()
            cli.install(old_archive, old_manifest, home=local_home,
                        platform=local_platform, effective_uid=501)
            unused_transport = FakeReleaseTransport({})
            cli.run_installed([
                "update", "--archive", str(new_archive),
                "--manifest", str(new_manifest),
            ], home=local_home, platform=local_platform,
                transport=unused_transport, effective_uid=501)
            self.assertEqual(unused_transport.calls, [])
            self.assertEqual(
                os.readlink(local_home / ".local/share/lingtai-desktop/current"),
                "versions/0.1.6",
            )

            before_failure = tree_snapshot(managed)
            failing_platform = FakePlatform()
            failing_platform.fail = "verifier"
            failing_transport = official_release_transport(new_archive, new_manifest, "0.1.6")
            with self.assertRaisesRegex(cli.DesktopCLIError, "verifier failure"):
                cli.run_installed(
                    ["update"], home=home, platform=failing_platform,
                    transport=failing_transport, effective_uid=501,
                )
            self.assertEqual(tree_snapshot(managed), before_failure)
            downloaded = [call for call in failing_platform.calls if call[0] == "verify"]
            self.assertEqual(len(downloaded), 1)
            self.assertFalse(Path(downloaded[0][1]).parent.exists())

            with self.assertRaisesRegex(cli.DesktopCLIError, "mutually exclusive"):
                cli.run_installed([
                    "update", "--version", "0.1.6",
                    "--archive", str(new_archive), "--manifest", str(new_manifest),
                ], home=home, platform=platform, transport=transport,
                    effective_uid=501)

    def test_official_downloader_rejects_untrusted_metadata_redirects_and_stream_bounds(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            release_root = root / "release"
            release_root.mkdir()
            archive, manifest = write_artifacts(release_root, "0.1.6")
            exact_url = cli.OFFICIAL_RELEASE_TAG_URL.format("0.1.6")
            archive_url = (
                "https://github.com/Lingtai-AI/lingtai-desktop/releases/download/"
                f"v0.1.6/{archive.name}"
            )
            manifest_url = (
                "https://github.com/Lingtai-AI/lingtai-desktop/releases/download/"
                f"v0.1.6/{manifest.name}"
            )

            base_transport = official_release_transport(archive, manifest, "0.1.6")
            base_metadata = json.loads(base_transport.routes[exact_url][2])

            metadata_cases: list[tuple[str, dict[str, object] | bytes | int]] = []
            for label, replacement in (
                ("draft", {**base_metadata, "draft": True}),
                ("prerelease", {**base_metadata, "prerelease": True}),
                ("wrong-tag", {**base_metadata, "tag_name": "v0.1.7"}),
                ("missing-asset", {**base_metadata, "assets": base_metadata["assets"][:1]}),
                ("duplicate-asset", {**base_metadata, "assets": [
                    *base_metadata["assets"], base_metadata["assets"][0],
                ]}),
            ):
                metadata_cases.append((label, replacement))
            for label, bad_url in (
                ("http-asset", archive_url.replace("https://", "http://")),
                ("foreign-asset", archive_url.replace("github.com", "example.com")),
                ("port-asset", archive_url.replace("github.com", "github.com:443")),
                ("mismatched-name", archive_url.replace(archive.name, "other.tar.gz")),
            ):
                changed = json.loads(json.dumps(base_metadata))
                changed["assets"][0]["browser_download_url"] = bad_url
                metadata_cases.append((label, changed))
            oversized = json.loads(json.dumps(base_metadata))
            oversized["assets"][0]["size"] = cli.MAX_ARCHIVE_BYTES + 1
            metadata_cases.append(("archive-asset-size", oversized))
            oversized_manifest = json.loads(json.dumps(base_metadata))
            oversized_manifest["assets"][1]["size"] = cli.MAX_MANIFEST_BYTES + 1
            metadata_cases.append(("manifest-asset-size", oversized_manifest))
            metadata_cases.extend((
                ("duplicate-json-key", (
                    b'{"tag_name":"v0.1.6","tag_name":"v0.1.7",'
                    b'"draft":false,"prerelease":false,"assets":[]}'
                )),
                ("metadata-status", 429),
                ("metadata-bound", b"x" * (cli.MAX_RELEASE_METADATA_BYTES + 1)),
            ))

            stream_cases: list[tuple[str, FakeReleaseTransport]] = []
            partial = official_release_transport(archive, manifest, "0.1.6")
            archive_final = partial.routes[archive_url][1]["Location"]
            partial.routes[archive_final] = (
                206, {"Content-Length": str(archive.stat().st_size)}, archive.read_bytes(),
            )
            stream_cases.append(("partial-status", partial))

            for label, location in (
                ("http-redirect", "http://release-assets.githubusercontent.com/file"),
                ("foreign-redirect", "https://example.com/file"),
                ("credential-redirect", "https://user:secret@release-assets.githubusercontent.com/file"),
                ("port-redirect", "https://release-assets.githubusercontent.com:444/file"),
                ("malformed-redirect", "https://[invalid"),
            ):
                candidate = official_release_transport(archive, manifest, "0.1.6")
                candidate.routes[archive_url] = (302, {"Location": location}, b"")
                stream_cases.append((label, candidate))

            length_mismatch = official_release_transport(archive, manifest, "0.1.6")
            final_url = length_mismatch.routes[archive_url][1]["Location"]
            length_mismatch.routes[final_url] = (
                200, {"Content-Length": str(archive.stat().st_size + 1)}, archive.read_bytes(),
            )
            stream_cases.append(("content-length", length_mismatch))

            truncated = official_release_transport(archive, manifest, "0.1.6")
            final_url = truncated.routes[archive_url][1]["Location"]
            truncated.routes[final_url] = (
                200, {"Content-Length": str(archive.stat().st_size)}, archive.read_bytes()[:-1],
            )
            stream_cases.append(("truncated", truncated))

            overrun = official_release_transport(archive, manifest, "0.1.6")
            final_url = overrun.routes[archive_url][1]["Location"]
            overrun.routes[final_url] = (200, {}, archive.read_bytes() + b"overrun")
            stream_cases.append(("overrun", overrun))

            manifest_truncated = official_release_transport(archive, manifest, "0.1.6")
            manifest_final = manifest_truncated.routes[manifest_url][1]["Location"]
            manifest_truncated.routes[manifest_final] = (
                200, {"Content-Length": str(manifest.stat().st_size)},
                manifest.read_bytes()[:-1],
            )
            stream_cases.append(("manifest-truncated", manifest_truncated))

            manifest_overrun = official_release_transport(archive, manifest, "0.1.6")
            manifest_final = manifest_overrun.routes[manifest_url][1]["Location"]
            manifest_overrun.routes[manifest_final] = (200, {}, manifest.read_bytes() + b"overrun")
            stream_cases.append(("manifest-overrun", manifest_overrun))

            downloads = root / "downloads"
            downloads.mkdir()
            case_number = 0
            with mock.patch.object(cli.tempfile, "tempdir", str(downloads)):
                for label, replacement in metadata_cases:
                    case_number += 1
                    with self.subTest(case=label):
                        candidate = official_release_transport(archive, manifest, "0.1.6")
                        if isinstance(replacement, int):
                            candidate.routes[exact_url] = (replacement, {}, b"")
                        else:
                            body = replacement if isinstance(replacement, bytes) else json.dumps(replacement).encode()
                            candidate.routes[exact_url] = (
                                200, {"Content-Length": str(len(body))}, body,
                            )
                        home = root / f"metadata-home-{case_number}"
                        home.mkdir()
                        before = tree_snapshot(home)
                        with self.assertRaises(cli.DesktopCLIError) as raised:
                            cli.install_official(
                                "0.1.6", home=home, platform=FakePlatform(),
                                transport=candidate, effective_uid=501,
                            )
                        self.assertNotIn("token=", str(raised.exception))
                        self.assertEqual(tree_snapshot(home), before)
                        self.assertEqual(list(downloads.iterdir()), [])

                for label, candidate in stream_cases:
                    case_number += 1
                    with self.subTest(case=label):
                        home = root / f"stream-home-{case_number}"
                        home.mkdir()
                        before = tree_snapshot(home)
                        with self.assertRaises(cli.DesktopCLIError) as raised:
                            cli.install_official(
                                "0.1.6", home=home, platform=FakePlatform(),
                                transport=candidate, effective_uid=501,
                            )
                        self.assertNotIn("token=", str(raised.exception))
                        self.assertEqual(tree_snapshot(home), before)
                        self.assertEqual(list(downloads.iterdir()), [])

    def test_recursive_json_is_bounded_for_automatic_and_explicit_discovery(self) -> None:
        nested = b"[" * 60_000
        for mode in ("explicit", "automatic"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                transport = FakeReleaseTransport({
                    cli.OFFICIAL_LATEST_RELEASE_URL: (
                        200, {"Content-Length": str(len(nested))}, nested,
                    ),
                })
                if mode == "explicit":
                    with self.assertRaisesRegex(
                            cli.DesktopCLIError,
                            "official release metadata is not bounded valid JSON",
                    ):
                        cli.discover_official_release(transport=transport)
                    continue

                home = root / "home"
                home.mkdir()
                archive, manifest = write_artifacts(root, "0.1.5")
                platform = FakePlatform()
                cli.install(archive, manifest, home=home,
                            platform=platform, effective_uid=501)
                managed = home / ".local/share/lingtai-desktop"
                before = tree_snapshot(managed)
                output: list[str] = []

                self.assertEqual(cli.run_installed(
                    ["open"], home=home, platform=platform, transport=transport,
                    clock=lambda: 1_000.0, tty=lambda: False,
                    output=output.append, effective_uid=501,
                ), 0)
                old_app = managed / "versions/0.1.5/LingTai.app"
                self.assertEqual(platform.calls[-1], ["/usr/bin/open", str(old_app)])
                self.assertEqual(tree_snapshot(managed), before)
                self.assertFalse((managed / "update-check.json").exists())

    def test_recursive_json_is_bounded_for_manifest_receipt_and_cache(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest = write_artifacts(root, "0.1.5")
            with self.subTest(boundary="manifest"), mock.patch.object(
                    cli.json, "loads", side_effect=RecursionError("injected nested manifest"),
            ):
                with self.assertRaisesRegex(
                        cli.DesktopCLIError, "manifest must be bounded valid JSON",
                ):
                    cli.load_manifest(archive, manifest)

            home = root / "home"
            home.mkdir()
            cli.install(archive, manifest, home=home,
                        platform=FakePlatform(), effective_uid=501)
            paths = cli._paths(home)
            with self.subTest(boundary="receipt"), mock.patch.object(
                    cli.json, "loads", side_effect=RecursionError("injected nested receipt"),
            ):
                with self.assertRaisesRegex(cli.DesktopCLIError, "receipt is invalid"):
                    cli._read_receipt(paths, "0.1.5")

            paths.update_cache.write_text(json.dumps({
                "checked_at": 1_000,
                "latest_version": "0.1.6",
                "schema_version": cli.UPDATE_CACHE_SCHEMA,
            }))
            paths.update_cache.chmod(0o600)
            with self.subTest(boundary="cache"), mock.patch.object(
                    cli.json, "loads", side_effect=RecursionError("injected nested cache"),
            ):
                with self.assertRaisesRegex(
                        cli.DesktopCLIError, "managed update-check cache is invalid",
                ):
                    cli._read_update_cache(paths)

    def test_official_asset_owner_casing_is_tolerant_but_route_tail_is_exact(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest = write_artifacts(root, "0.1.6")
            exact_url = cli.OFFICIAL_RELEASE_TAG_URL.format("0.1.6")
            lower_owner = official_release_transport(archive, manifest, "0.1.6")
            metadata = json.loads(lower_owner.routes[exact_url][2])
            for asset in metadata["assets"]:
                asset["browser_download_url"] = asset["browser_download_url"].replace(
                    "/Lingtai-AI/lingtai-desktop/", "/lingtai-AI/lingtai-desktop/",
                )
            body = json.dumps(metadata).encode()
            lower_owner.routes[exact_url] = (
                200, {"Content-Length": str(len(body))}, body,
            )
            release = cli.discover_official_release("0.1.6", transport=lower_owner)
            self.assertIn("/lingtai-AI/lingtai-desktop/", release.archive.url)
            self.assertIn("/lingtai-AI/lingtai-desktop/", release.manifest.url)

            archive_url = metadata["assets"][0]["browser_download_url"]
            for label, bad_url in (
                ("tail", archive_url.replace("/releases/download/", "/release/download/")),
                ("tag", archive_url.replace("/v0.1.6/", "/V0.1.6/")),
                ("name", archive_url.replace(archive.name, f"wrong-{archive.name}")),
            ):
                with self.subTest(case=label):
                    candidate = official_release_transport(archive, manifest, "0.1.6")
                    changed = json.loads(candidate.routes[exact_url][2])
                    changed["assets"][0]["browser_download_url"] = bad_url
                    changed_body = json.dumps(changed).encode()
                    candidate.routes[exact_url] = (
                        200, {"Content-Length": str(len(changed_body))}, changed_body,
                    )
                    with self.assertRaisesRegex(
                            cli.DesktopCLIError,
                            "official release asset URL does not match its tag and name",
                    ):
                        cli.discover_official_release("0.1.6", transport=candidate)

    def test_automatic_metadata_redirects_and_reads_share_one_total_deadline(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest = write_artifacts(root, "0.1.5")
            home = root / "home"
            home.mkdir()
            platform = FakePlatform()
            cli.install(archive, manifest, home=home,
                        platform=platform, effective_uid=501)
            managed = home / ".local/share/lingtai-desktop"
            before = tree_snapshot(managed)

            release_archive, release_manifest = write_artifacts(root, "0.1.6")
            complete = official_release_transport(release_archive, release_manifest, "0.1.6")
            metadata = complete.routes[cli.OFFICIAL_LATEST_RELEASE_URL][2]
            redirected_url = "https://api.github.com/slow-official-release"
            transport = FakeReleaseTransport({
                cli.OFFICIAL_LATEST_RELEASE_URL: (
                    302, {"Location": redirected_url}, b"",
                ),
                redirected_url: (200, {"Content-Length": str(len(metadata))}, metadata),
            })
            monotonic_values = iter((100.0, 100.25, 100.5, 100.75, 101.0, 101.25, 102.1))
            with mock.patch.object(cli.time, "monotonic", side_effect=monotonic_values):
                self.assertEqual(cli.run_installed(
                    ["open"], home=home, platform=platform, transport=transport,
                    clock=lambda: 1_000.0, tty=lambda: False,
                    output=lambda _: None, effective_uid=501,
                ), 0)

            old_app = managed / "versions/0.1.5/LingTai.app"
            self.assertEqual(platform.calls[-1], ["/usr/bin/open", str(old_app)])
            self.assertEqual(tree_snapshot(managed), before)
            self.assertFalse((managed / "update-check.json").exists())
            self.assertEqual([call[0] for call in transport.calls], [
                cli.OFFICIAL_LATEST_RELEASE_URL, redirected_url,
            ])
            self.assertGreater(transport.calls[0][2], transport.calls[1][2])
            self.assertLessEqual(transport.calls[0][2], cli.AUTOMATIC_RELEASE_TIMEOUT)

            explicit = FakeReleaseTransport({
                cli.OFFICIAL_LATEST_RELEASE_URL: (
                    302, {"Location": redirected_url}, b"",
                ),
                redirected_url: (200, {"Content-Length": str(len(metadata))}, metadata),
            })
            with mock.patch.object(
                    cli.time, "monotonic",
                    side_effect=AssertionError("explicit discovery used automatic deadline"),
            ):
                cli.discover_official_release(transport=explicit)
            self.assertTrue(all(
                call[2] == cli.EXPLICIT_RELEASE_TIMEOUT for call in explicit.calls
            ))

    def test_confirmed_update_temp_directory_failure_continues_old_command(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            old_archive, old_manifest = write_artifacts(root, "0.1.5")
            release_root = root / "release"
            release_root.mkdir()
            new_archive, new_manifest = write_artifacts(release_root, "0.1.6")
            home = root / "home"
            home.mkdir()
            platform = FakePlatform()
            cli.install(old_archive, old_manifest, home=home,
                        platform=platform, effective_uid=501)
            managed = home / ".local/share/lingtai-desktop"
            before = tree_snapshot(managed)
            output: list[str] = []

            with mock.patch.object(
                    cli.tempfile, "mkdtemp", side_effect=OSError("injected no tmp"),
            ):
                self.assertEqual(cli.run_installed(
                    ["open"], home=home, platform=platform,
                    transport=official_release_transport(
                        new_archive, new_manifest, "0.1.6",
                    ),
                    clock=lambda: 1_000.0, tty=lambda: True, prompt=lambda _: "yes",
                    output=output.append, effective_uid=501,
                ), 0)

            old_app = managed / "versions/0.1.5/LingTai.app"
            self.assertEqual(platform.calls[-1], ["/usr/bin/open", str(old_app)])
            self.assertEqual(tree_snapshot(managed), before)
            self.assertTrue(any(
                "Update failed" in line and "continuing" in line for line in output
            ))

    def test_explicit_update_cache_record_failure_warns_after_successful_install(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            old_archive, old_manifest = write_artifacts(root, "0.1.5")
            release_root = root / "release"
            release_root.mkdir()
            new_archive, new_manifest = write_artifacts(release_root, "0.1.6")
            home = root / "home"
            home.mkdir()
            platform = FakePlatform()
            cli.install(old_archive, old_manifest, home=home,
                        platform=platform, effective_uid=501)
            managed = home / ".local/share/lingtai-desktop"
            output: list[str] = []

            with mock.patch.object(
                    cli, "_write_update_cache",
                    side_effect=cli.DesktopCLIError("injected cache record failure"),
            ):
                self.assertEqual(cli.run_installed(
                    ["update"], home=home, platform=platform,
                    transport=official_release_transport(
                        new_archive, new_manifest, "0.1.6",
                    ),
                    clock=lambda: 1_000.0, output=output.append,
                    effective_uid=501,
                ), 0)

            self.assertEqual(os.readlink(managed / "current"), "versions/0.1.6")
            self.assertTrue(any(
                "updated LingTai Desktop to 0.1.6" in line
                and "cache was not recorded" in line
                and "injected cache record failure" in line
                for line in output
            ))

            cache = managed / "update-check.json"
            cache.write_text("{invalid")
            cache.chmod(0o600)
            before = tree_snapshot(managed)
            unused_transport = official_release_transport(new_archive, new_manifest, "0.1.6")
            with self.assertRaisesRegex(cli.DesktopCLIError, "cache is invalid"):
                cli.run_installed(
                    ["update"], home=home, platform=platform,
                    transport=unused_transport, effective_uid=501,
                )
            self.assertEqual(unused_transport.calls, [])
            self.assertEqual(tree_snapshot(managed), before)

    def test_normal_commands_use_cached_offer_and_only_confirmed_tty_updates(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            release_root = root / "release"
            release_root.mkdir()
            old_archive, old_manifest = write_artifacts(root, "0.1.5")
            new_archive, new_manifest = write_artifacts(release_root, "0.1.6")
            home = root / "home"
            home.mkdir()
            platform = FakePlatform()
            cli.install(old_archive, old_manifest, home=home,
                        platform=platform, effective_uid=501)
            managed = home / ".local/share/lingtai-desktop"

            refreshed = official_release_transport(new_archive, new_manifest, "0.1.6")
            first_output: list[str] = []
            cli.run_installed(
                ["version"], home=home, platform=platform,
                transport=refreshed, output=first_output.append,
                clock=lambda: 1_000.0, tty=lambda: False,
                prompt=lambda _: (_ for _ in ()).throw(AssertionError("non-TTY prompted")),
            )
            self.assertEqual([call[0] for call in refreshed.calls], [cli.OFFICIAL_LATEST_RELEASE_URL])
            cache = managed / "update-check.json"
            self.assertEqual(stat.S_IMODE(cache.stat().st_mode), 0o600)
            self.assertEqual(json.loads(cache.read_text()), {
                "checked_at": 1000, "latest_version": "0.1.6", "schema_version": 1,
            })
            self.assertTrue(any("Update available: 0.1.6" in line for line in first_output))
            self.assertEqual(os.readlink(managed / "current"), "versions/0.1.5")

            for arguments in (["open"], ["foreground", "--", "--probe"],
                              ["version"], ["doctor"]):
                with self.subTest(command=arguments[0]):
                    no_network = FakeReleaseTransport({})
                    output: list[str] = []
                    cli.run_installed(
                        arguments, home=home, platform=platform,
                        transport=no_network, output=output.append,
                        clock=lambda: 1_001.0, tty=lambda: False,
                        prompt=lambda _: (_ for _ in ()).throw(AssertionError("non-TTY prompted")),
                    )
                    self.assertEqual(no_network.calls, [])
                    self.assertTrue(any("Update available: 0.1.6" in line for line in output))
            self.assertTrue(platform.calls[-2][0] == "/usr/bin/open" or
                            any(call[0] == "/usr/bin/open" for call in platform.calls))
            self.assertEqual(platform.exec_calls[-1][-1], "--probe")

            before_declines = tree_snapshot(managed)
            for answer in ("", "n", "not now", EOFError()):
                with self.subTest(answer=type(answer).__name__ if isinstance(answer, EOFError) else answer):
                    prompts: list[str] = []

                    def decline(message: str, value: str | EOFError = answer) -> str:
                        prompts.append(message)
                        if isinstance(value, EOFError):
                            raise value
                        return value

                    cli.run_installed(
                        ["version"], home=home, platform=platform,
                        transport=FakeReleaseTransport({}), output=lambda _: None,
                        clock=lambda: 1_001.0, tty=lambda: True, prompt=decline,
                    )
                    self.assertEqual(len(prompts), 1)
                    self.assertEqual(tree_snapshot(managed), before_declines)

            confirmed_transport = official_release_transport(new_archive, new_manifest, "0.1.6")
            confirmed_output: list[str] = []
            cli.run_installed(
                ["version"], home=home, platform=platform,
                transport=confirmed_transport, output=confirmed_output.append,
                clock=lambda: 1_002.0, tty=lambda: True, prompt=lambda _: "YES",
                effective_uid=501,
            )
            self.assertEqual(os.readlink(managed / "current"), "versions/0.1.6")
            self.assertIn("version: 0.1.6", confirmed_output)
            self.assertEqual(json.loads(cache.read_text())["latest_version"], "0.1.6")

            explicit_home = root / "explicit-home"
            explicit_home.mkdir()
            explicit_platform = FakePlatform()
            cli.install(old_archive, old_manifest, home=explicit_home,
                        platform=explicit_platform, effective_uid=501)
            cli.run_installed(
                ["update"], home=explicit_home, platform=explicit_platform,
                transport=official_release_transport(new_archive, new_manifest, "0.1.6"),
                output=lambda _: None, clock=lambda: 2_000.0, tty=lambda: True,
                prompt=lambda _: (_ for _ in ()).throw(AssertionError("explicit update prompted")),
                effective_uid=501,
            )
            explicit_cache = explicit_home / ".local/share/lingtai-desktop/update-check.json"
            self.assertEqual(json.loads(explicit_cache.read_text())["checked_at"], 2000)
            cli.run_installed(
                ["uninstall", "--version", "0.1.5"], home=explicit_home,
                platform=explicit_platform, transport=FakeReleaseTransport({}),
                tty=lambda: True,
                prompt=lambda _: (_ for _ in ()).throw(AssertionError("uninstall prompted")),
                effective_uid=501, output=lambda _: None,
            )

            failure_home = root / "failure-home"
            failure_home.mkdir()
            cli.install(old_archive, old_manifest, home=failure_home,
                        platform=FakePlatform(), effective_uid=501)
            initial_check = official_release_transport(new_archive, new_manifest, "0.1.6")
            cli.run_installed(
                ["version"], home=failure_home, platform=FakePlatform(),
                transport=initial_check, output=lambda _: None,
                clock=lambda: 3_000.0, tty=lambda: False,
            )
            failure_managed = failure_home / ".local/share/lingtai-desktop"
            failure_before = tree_snapshot(failure_managed)
            failing_platform = FakePlatform()
            failing_platform.fail = "verifier"
            failure_output: list[str] = []
            cli.run_installed(
                ["open"], home=failure_home, platform=failing_platform,
                transport=official_release_transport(new_archive, new_manifest, "0.1.6"),
                output=failure_output.append, clock=lambda: 3_001.0,
                tty=lambda: True, prompt=lambda _: "y", effective_uid=501,
            )
            self.assertEqual(tree_snapshot(failure_managed), failure_before)
            old_app = failure_managed / "versions/0.1.5/LingTai.app"
            self.assertEqual(failing_platform.calls[-1], ["/usr/bin/open", str(old_app)])
            self.assertTrue(any("Update failed" in line and "continuing" in line
                                for line in failure_output))

            stale_cache_before = (failure_managed / "update-check.json").read_bytes()
            timeout_transport = mock.Mock()
            timeout_transport.open.side_effect = TimeoutError("offline timeout")
            connection_transport = mock.Mock()
            connection_transport.open.side_effect = ConnectionError("offline")
            for label, failed_check in (
                ("timeout", timeout_transport),
                ("offline", connection_transport),
                ("rate-limit", FakeReleaseTransport({
                    cli.OFFICIAL_LATEST_RELEASE_URL: (429, {}, b"rate limited"),
                })),
                ("malformed", FakeReleaseTransport({
                    cli.OFFICIAL_LATEST_RELEASE_URL: (200, {"Content-Length": "1"}, b"{"),
                })),
            ):
                with self.subTest(check_failure=label):
                    offline_output: list[str] = []
                    cli.run_installed(
                        ["version"], home=failure_home, platform=FakePlatform(),
                        transport=failed_check, output=offline_output.append,
                        clock=lambda: 3_000.0 + cli.DEFAULT_UPDATE_CHECK_INTERVAL + 1,
                        tty=lambda: False,
                    )
                    self.assertIn("version: 0.1.5", offline_output)
                    self.assertEqual((failure_managed / "update-check.json").read_bytes(),
                                     stale_cache_before)

    def test_version_policy_bounds_every_remote_and_managed_boundary(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            archive, manifest = write_artifacts(root, "0.1.5")
            cli.install(archive, manifest, home=home,
                        platform=FakePlatform(), effective_uid=501)
            managed = home / ".local/share/lingtai-desktop"
            before = tree_snapshot(managed)

            huge = f"{'1' * 5000}.2.3"
            archive_name = f"LingTai-{huge}-macOS-universal.app.tar.gz"
            manifest_name = f"LingTai-{huge}-macOS-universal.app.manifest.json"
            metadata = json.dumps({
                "tag_name": f"v{huge}",
                "draft": False,
                "prerelease": False,
                "assets": [
                    {
                        "name": archive_name,
                        "browser_download_url": (
                            "https://github.com/Lingtai-AI/lingtai-desktop/releases/"
                            f"download/v{huge}/{archive_name}"
                        ),
                        "size": 1,
                    },
                    {
                        "name": manifest_name,
                        "browser_download_url": (
                            "https://github.com/Lingtai-AI/lingtai-desktop/releases/"
                            f"download/v{huge}/{manifest_name}"
                        ),
                        "size": 1,
                    },
                ],
            }).encode()
            remote = FakeReleaseTransport({
                cli.OFFICIAL_LATEST_RELEASE_URL: (
                    200, {"Content-Length": str(len(metadata))}, metadata,
                ),
            })
            output: list[str] = []
            self.assertEqual(cli.run_installed(
                ["version"], home=home, platform=FakePlatform(),
                transport=remote, output=output.append,
                clock=lambda: 1_000.0, tty=lambda: False,
            ), 0)
            self.assertIn("version: 0.1.5", output)
            self.assertEqual(tree_snapshot(managed), before)
            self.assertFalse((managed / "update-check.json").exists())

            explicit = FakeReleaseTransport({})
            with self.assertRaises(cli.DesktopCLIError):
                cli.discover_official_release(huge, transport=explicit)
            self.assertEqual(explicit.calls, [])

            long_component = "1234567890.1.2"
            long_archive, long_manifest = write_artifacts(root, long_component)
            with self.assertRaises(cli.DesktopCLIError):
                cli.load_manifest(long_archive, long_manifest)
            with self.assertRaises(cli.DesktopCLIError):
                cli._version_tuple(long_component)

            cache = managed / "update-check.json"
            cache.write_text(json.dumps({
                "checked_at": 1000,
                "latest_version": long_component,
                "schema_version": cli.UPDATE_CACHE_SCHEMA,
            }))
            cache.chmod(0o600)
            with self.assertRaises(cli.DesktopCLIError):
                cli._read_update_cache(cli._paths(home))

    def test_invalid_normal_command_syntax_has_no_side_effects(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            old_archive, old_manifest = write_artifacts(root, "0.1.5")
            release_root = root / "release"
            release_root.mkdir()
            new_archive, new_manifest = write_artifacts(release_root, "0.1.6")

            for command in ("open", "version", "doctor"):
                with self.subTest(command=command):
                    home = root / f"{command}-home"
                    home.mkdir()
                    platform = FakePlatform()
                    cli.install(old_archive, old_manifest, home=home,
                                platform=platform, effective_uid=501)
                    before = tree_snapshot(home)
                    platform_calls = list(platform.calls)
                    transport = official_release_transport(
                        new_archive, new_manifest, "0.1.6",
                    )
                    prompts: list[str] = []
                    with self.assertRaisesRegex(
                            cli.DesktopCLIError, f"{command} takes no arguments",
                    ):
                        cli.run_installed(
                            [command, "unexpected"], home=home, platform=platform,
                            transport=transport, output=lambda _: None,
                            clock=lambda: 1_000.0, tty=lambda: True,
                            prompt=lambda message: prompts.append(message) or "n",
                            effective_uid=501,
                        )
                    self.assertEqual(transport.calls, [])
                    self.assertEqual(prompts, [])
                    self.assertEqual(platform.calls, platform_calls)
                    self.assertEqual(tree_snapshot(home), before)

    def test_update_cache_hardlink_publication_race_rolls_back_owned_name(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            archive, manifest = write_artifacts(root, "0.1.5")
            cli.install(archive, manifest, home=home,
                        platform=FakePlatform(), effective_uid=501)
            paths = cli._paths(home)
            managed_before = tree_snapshot(paths.root)
            outside = root / "racer-owned-cache-link"
            real_link = os.link

            def inject_hardlink(source: Path, destination: Path, **kwargs: object) -> None:
                real_link(source, destination, **kwargs)
                real_link(destination, outside)

            with mock.patch.object(cli.os, "link", side_effect=inject_hardlink):
                with self.assertRaises(cli.DesktopCLIError):
                    cli._write_update_cache(paths, "0.1.6", 1_000)

            self.assertEqual(tree_snapshot(paths.root), managed_before)
            self.assertFalse(paths.update_cache.exists())
            self.assertTrue(outside.is_file())
            self.assertEqual(outside.stat().st_nlink, 1)
            self.assertEqual(json.loads(outside.read_text())["latest_version"], "0.1.6")

    def test_update_cache_tamper_is_non_destructive_and_fully_owned_by_uninstall(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            release_root = root / "release"
            release_root.mkdir()
            archive, manifest = write_artifacts(root, "0.1.5")
            new_archive, new_manifest = write_artifacts(release_root, "0.1.6")

            def installed_with_cache(name: str) -> tuple[Path, Path]:
                home = root / name
                home.mkdir()
                cli.install(archive, manifest, home=home,
                            platform=FakePlatform(), effective_uid=501)
                cli.run_installed(
                    ["version"], home=home, platform=FakePlatform(),
                    transport=official_release_transport(new_archive, new_manifest, "0.1.6"),
                    clock=lambda: 10_000.0, tty=lambda: False, output=lambda _: None,
                )
                managed = home / ".local/share/lingtai-desktop"
                return home, managed

            valid_home, valid_managed = installed_with_cache("valid-home")
            cli.doctor(home=valid_home)
            cli.uninstall_all(home=valid_home, effective_uid=501)
            self.assertFalse(valid_managed.exists())
            self.assertFalse((valid_home / ".local/bin/lingtai-desktop").exists())

            for corruption in ("symlink", "hardlink", "mode", "schema", "oversize"):
                with self.subTest(corruption=corruption):
                    home, managed = installed_with_cache(f"{corruption}-home")
                    cache = managed / "update-check.json"
                    outside = root / f"{corruption}-outside-cache"
                    outside.write_bytes(cache.read_bytes())
                    outside.chmod(0o600)
                    if corruption == "symlink":
                        cache.unlink()
                        cache.symlink_to(outside)
                    elif corruption == "hardlink":
                        cache.unlink()
                        os.link(outside, cache)
                    elif corruption == "mode":
                        cache.chmod(0o644)
                    elif corruption == "schema":
                        value = json.loads(cache.read_text())
                        value["unexpected"] = True
                        cache.write_text(json.dumps(value))
                        cache.chmod(0o600)
                    else:
                        cache.write_bytes(b"x" * (cli.MAX_UPDATE_CACHE_BYTES + 1))
                        cache.chmod(0o600)

                    managed_before = tree_snapshot(managed)
                    outside_before = tree_snapshot(outside)
                    ordinary_output: list[str] = []
                    no_network = FakeReleaseTransport({})
                    cli.run_installed(
                        ["version"], home=home, platform=FakePlatform(),
                        transport=no_network, clock=lambda: 10_001.0,
                        tty=lambda: False, output=ordinary_output.append,
                    )
                    self.assertEqual(no_network.calls, [])
                    self.assertIn("version: 0.1.5", ordinary_output)
                    self.assertFalse(any("Update available" in line for line in ordinary_output))
                    self.assertEqual(tree_snapshot(managed), managed_before)
                    self.assertEqual(tree_snapshot(outside), outside_before)

                    with self.assertRaises(cli.DesktopCLIError):
                        cli.doctor(home=home)
                    with self.assertRaises(cli.DesktopCLIError):
                        cli.uninstall_all(home=home, effective_uid=501)
                    self.assertEqual(tree_snapshot(managed), managed_before)
                    self.assertEqual(tree_snapshot(outside), outside_before)

    def test_existing_shared_parent_modes_and_contents_are_preserved(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            shared = (home / ".local", home / ".local/bin", home / ".local/share")
            for directory in shared:
                directory.mkdir(parents=True, exist_ok=True)
                directory.chmod(0o755)
            unrelated = home / ".local/share/unrelated.txt"
            unrelated.write_text("keep me")
            before = tuple(stat.S_IMODE(path.stat().st_mode) for path in shared), unrelated.read_bytes()
            archive, manifest = write_artifacts(root)
            cli.install(archive, manifest, home=home,
                        platform=FakePlatform(), effective_uid=501)
            self.assertEqual(
                (tuple(stat.S_IMODE(path.stat().st_mode) for path in shared), unrelated.read_bytes()),
                before,
            )
            managed = home / ".local/share/lingtai-desktop"
            self.assertTrue(all(stat.S_IMODE(path.stat().st_mode) == 0o700 for path in (
                managed, managed / "support", managed / "support/versions",
                managed / "versions", managed / "receipts",
            )))

    def test_publication_preparation_failure_leaves_no_destination(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "staged"
            destination = root / "published"
            source.write_bytes(b"complete")
            with mock.patch.object(cli.os, "chmod", side_effect=PermissionError("injected")):
                with self.assertRaises(cli.DesktopCLIError):
                    cli._publish_file_exclusive(source, destination, 0o600)
            self.assertFalse(destination.exists())

    def test_uninstall_refuses_symlinked_managed_root_without_touching_outside_or_launcher(self) -> None:
        for command in ("version", "all"):
            with self.subTest(command=command), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                home = root / "home"
                home.mkdir()
                archive, manifest = write_artifacts(root)
                cli.install(archive, manifest, home=home,
                            platform=FakePlatform(), effective_uid=501)
                managed = home / ".local/share/lingtai-desktop"
                outside = root / "outside-managed"
                managed.rename(outside)
                managed.symlink_to(outside, target_is_directory=True)
                launcher = home / ".local/bin/lingtai-desktop"
                outside_before, launcher_before = tree_snapshot(outside), launcher.read_bytes()
                with self.assertRaisesRegex(cli.DesktopCLIError, "symlink"):
                    if command == "version":
                        cli.uninstall_version("0.1.5", home=home, effective_uid=501)
                    else:
                        cli.uninstall_all(home=home, effective_uid=501)
                self.assertEqual(tree_snapshot(outside), outside_before)
                self.assertEqual(launcher.read_bytes(), launcher_before)

    def test_uninstall_all_fully_preflights_unknown_root_and_later_tamper(self) -> None:
        for corruption in ("unknown-root", "later-version-tamper"):
            with self.subTest(corruption=corruption), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                home = root / "home"
                home.mkdir()
                platform = FakePlatform()
                archive1, manifest1 = write_artifacts(root, "0.1.5")
                cli.install(archive1, manifest1, home=home,
                            platform=platform, effective_uid=501)
                if corruption == "unknown-root":
                    (home / ".local/share/lingtai-desktop/KEEP").write_text("unknown")
                else:
                    newer = root / "newer"
                    newer.mkdir()
                    archive2, manifest2 = write_artifacts(newer, "0.1.6")
                    cli.install(archive2, manifest2, home=home,
                                platform=platform, effective_uid=501, update=True)
                    executable = home / ".local/share/lingtai-desktop/versions/0.1.6/LingTai.app/Contents/MacOS/LingTai"
                    executable.chmod(0o700)
                managed = home / ".local/share/lingtai-desktop"
                launcher = home / ".local/bin/lingtai-desktop"
                before, launcher_before = tree_snapshot(managed), launcher.read_bytes()
                with self.assertRaises(cli.DesktopCLIError):
                    cli.uninstall_all(home=home, effective_uid=501)
                self.assertEqual(tree_snapshot(managed), before)
                self.assertEqual(launcher.read_bytes(), launcher_before)

    def test_bootstrap_parser_uses_the_portable_archive_contract(self) -> None:
        with mock.patch.object(cli, "install", return_value="0.1.5"), \
             mock.patch.object(cli, "doctor", return_value=("0.1.5", {})), \
             mock.patch("builtins.print") as output:
            self.assertEqual(cli.bootstrap_main([
                "--archive", "/tmp/LingTai.app.tar.gz", "--manifest", "/tmp/LingTai.app.json",
            ]), 0)
        self.assertFalse(any("WARNING" in str(call) for call in output.call_args_list))

    def test_manifest_exact_schema_hash_size_state_and_symlink_refusal(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest = write_artifacts(root)
            parsed = cli.load_manifest(archive, manifest)
            self.assertEqual(parsed.version, "0.1.5")

            data = json.loads(manifest.read_text())
            data["unknown"] = True
            manifest.write_text(json.dumps(data))
            with self.assertRaisesRegex(cli.DesktopCLIError, "exact schema"):
                cli.load_manifest(archive, manifest)
            data.pop("unknown")
            data["archive_sha256"] = "0" * 64
            manifest.write_text(json.dumps(data))
            with self.assertRaisesRegex(cli.DesktopCLIError, "SHA-256"):
                cli.load_manifest(archive, manifest)
            manifest.unlink()
            manifest.symlink_to(root / "elsewhere")
            with self.assertRaisesRegex(cli.DesktopCLIError, "regular file, not a symlink"):
                cli.load_manifest(archive, manifest)

    def test_archive_uses_independent_verifier_and_install_layout_is_private(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            archive, manifest = write_artifacts(root)
            platform = FakePlatform()
            cli.install(archive, manifest, home=home, platform=platform, effective_uid=501)
            managed = home / ".local/share/lingtai-desktop"
            self.assertEqual(os.readlink(managed / "current"), "versions/0.1.5")
            self.assertTrue((managed / "versions/0.1.5/LingTai.app").is_dir())
            self.assertTrue((managed / "receipts/0.1.5.json").is_file())
            support_target = os.readlink(managed / "support/current")
            self.assertTrue((managed / "support" / support_target / "desktop_user_cli.py").is_file())
            self.assertFalse((managed / "cli").exists())
            launcher = home / ".local/bin/lingtai-desktop"
            self.assertEqual(stat.S_IMODE(launcher.stat().st_mode), 0o755)
            self.assertEqual(platform.calls[0][0], "verify")

    def test_same_version_update_is_verified_byte_identical_idempotence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            archive, manifest = write_artifacts(root)
            platform = FakePlatform()
            cli.install(archive, manifest, home=home, platform=platform, effective_uid=501)
            managed = home / ".local/share/lingtai-desktop"
            before = (os.readlink(managed / "current"), (managed / "receipts/0.1.5.json").read_bytes())
            cli.install(archive, manifest, home=home, platform=platform, effective_uid=501, update=True)
            self.assertEqual((os.readlink(managed / "current"), (managed / "receipts/0.1.5.json").read_bytes()), before)
            self.assertEqual([call[0] for call in platform.calls].count("verify"), 2)

    def test_launch_version_doctor_and_tamper_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            archive, manifest = write_artifacts(root)
            platform = FakePlatform()
            transport = unavailable_release_transport()
            cli.install(archive, manifest, home=home, platform=platform, effective_uid=501)
            out: list[str] = []
            cli.run_installed([], home=home, platform=platform, transport=transport,
                              tty=lambda: False, output=out.append)
            app = home / ".local/share/lingtai-desktop/versions/0.1.5/LingTai.app"
            self.assertEqual(platform.calls[-1], ["/usr/bin/open", str(app)])
            cli.run_installed(["foreground", "--", "--smoke"], home=home,
                              platform=platform, transport=transport,
                              tty=lambda: False, output=out.append)
            self.assertEqual(platform.exec_calls[-1], [str(app / "Contents/MacOS/LingTai"), "--smoke"])
            cli.run_installed(["version"], home=home, platform=platform,
                              transport=transport, tty=lambda: False, output=out.append)
            cli.run_installed(["doctor"], home=home, platform=platform,
                              transport=transport, tty=lambda: False, output=out.append)
            self.assertIn("INTEGRITY PASS", "\n".join(out))
            self.assertIn("archive binding", "\n".join(out))
            (app / "Contents/MacOS/LingTai").write_bytes(b"tampered")
            with self.assertRaisesRegex(cli.DesktopCLIError, "executable facts|bundle digest"):
                cli.run_installed(["open"], home=home, platform=platform,
                                  transport=transport, tty=lambda: False,
                                  output=out.append)

            make_app(app, "0.1.5")
            support = home / ".local/share/lingtai-desktop/support"
            verifier = support / os.readlink(support / "current") / "verify-app-archive.py"
            verifier.write_text("tampered verifier")
            verifier.chmod(0o600)
            with self.assertRaises(cli.DesktopCLIError):
                cli.run_installed(["doctor"], home=home, platform=platform,
                                  transport=transport, tty=lambda: False,
                                  output=out.append)

    def test_update_failure_matrix_preserves_old_current_and_owned_bytes(self) -> None:
        for failure in ("verifier", "extract", "receipt", "current"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                home = root / "home"
                home.mkdir()
                platform = FakePlatform()
                archive1, manifest1 = write_artifacts(root, "0.1.5")
                cli.install(archive1, manifest1, home=home, platform=platform, effective_uid=501)
                managed = home / ".local/share/lingtai-desktop"
                launcher = home / ".local/bin/lingtai-desktop"
                old = (os.readlink(managed / "current"), launcher.read_bytes(), (managed / "receipts/0.1.5.json").read_bytes())
                newer = root / "new"
                newer.mkdir()
                archive2, manifest2 = write_artifacts(newer, "0.1.6")
                platform.fail = failure if failure in {"verifier", "extract"} else None
                with mock.patch.object(cli, "_FAILPOINT", failure), self.assertRaises(cli.DesktopCLIError):
                    cli.install(archive2, manifest2, home=home, platform=platform, effective_uid=501, update=True)
                self.assertEqual((os.readlink(managed / "current"), launcher.read_bytes(), (managed / "receipts/0.1.5.json").read_bytes()), old)
                self.assertFalse((managed / "versions/0.1.6").exists())
                self.assertFalse((managed / "receipts/0.1.6.json").exists())

    def test_collision_symlink_root_traversal_uninstall_and_root_refusal(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            archive, manifest = write_artifacts(root)
            launcher = home / ".local/bin/lingtai-desktop"
            launcher.parent.mkdir(parents=True)
            launcher.write_text("unrelated")
            with self.assertRaisesRegex(cli.DesktopCLIError, "unrelated launcher"):
                cli.install(archive, manifest, home=home, platform=FakePlatform(), effective_uid=501)
            launcher.unlink()
            with self.assertRaisesRegex(cli.DesktopCLIError, "effective uid 0"):
                cli.install(archive, manifest, home=home, platform=FakePlatform(), effective_uid=0)
            managed = home / ".local/share/lingtai-desktop"
            import shutil
            shutil.rmtree(managed)
            managed.symlink_to(root / "outside")
            with self.assertRaisesRegex(cli.DesktopCLIError, "symlink"):
                cli.install(archive, manifest, home=home, platform=FakePlatform(), effective_uid=501)

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            archive, manifest = write_artifacts(root)
            platform = FakePlatform()
            cli.install(archive, manifest, home=home, platform=platform, effective_uid=501)
            with self.assertRaisesRegex(cli.DesktopCLIError, "safe x.y.z"):
                cli.uninstall_version("../../outside", home=home, effective_uid=501)
            cli.uninstall_version("0.1.5", home=home, effective_uid=501)
            self.assertFalse((home / ".local/share/lingtai-desktop/versions/0.1.5").exists())
            archive, manifest = write_artifacts(root, "0.1.6")
            cli.install(archive, manifest, home=home, platform=platform, effective_uid=501)
            cli.uninstall_all(home=home, effective_uid=501)
            self.assertFalse(home.joinpath(".local/bin/lingtai-desktop").exists())
            self.assertFalse(home.joinpath(".local/share/lingtai-desktop").exists())

    def test_support_generation_models_are_canonical_fail_closed_and_anti_rollback(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            payloads = {
                "desktop_user_cli.py": b"def installed_main(argv=None):\n    return 0\n",
                "verify-app-archive.py": b"# independent verifier fixture\n",
            }

            def publish_fixture(parent: Path, version: str = "1.2.0",
                                module_suffix: bytes = b"") -> Path:
                candidate = dict(payloads)
                candidate["desktop_user_cli.py"] += module_suffix
                manifest_bytes = cli.build_support_manifest_bytes(
                    version, f"v{version}", candidate,
                )
                manifest_value = json.loads(manifest_bytes)
                generation = parent / manifest_value["generation_id"]
                generation.mkdir(parents=True, mode=0o700)
                generation.chmod(0o700)
                (generation / "support-manifest.json").write_bytes(manifest_bytes)
                for name, content in candidate.items():
                    (generation / name).write_bytes(content)
                for path in generation.iterdir():
                    path.chmod(0o600)
                return generation

            generation = publish_fixture(root / "valid")
            first_bytes = (generation / "support-manifest.json").read_bytes()
            second_bytes = cli.build_support_manifest_bytes(
                "1.2.0", "v1.2.0", payloads,
            )
            self.assertEqual(first_bytes, second_bytes)
            self.assertTrue(first_bytes.endswith(b"\n"))
            parsed = cli.parse_support_manifest(first_bytes)
            validated = cli.validate_support_generation(generation)
            self.assertEqual(parsed, validated.manifest)
            self.assertEqual(parsed.generation_id, generation.name)
            self.assertRegex(parsed.generation_id, r"^1\.2\.0-[0-9a-f]{12}$")
            self.assertEqual(
                [item.name for item in parsed.files],
                ["desktop_user_cli.py", "verify-app-archive.py"],
            )

            manifest_value = json.loads(first_bytes)
            malformed_manifests: list[dict[str, object]] = []
            unknown = json.loads(first_bytes)
            unknown["unknown"] = True
            malformed_manifests.append(unknown)
            traversal = json.loads(first_bytes)
            traversal["files"][0]["name"] = "../desktop_user_cli.py"
            malformed_manifests.append(traversal)
            duplicate_payload = json.loads(first_bytes)
            duplicate_payload["files"][1]["name"] = "desktop_user_cli.py"
            malformed_manifests.append(duplicate_payload)
            oversized = json.loads(first_bytes)
            oversized["files"][0]["size"] = cli.MAX_SUPPORT_PAYLOAD_BYTES + 1
            malformed_manifests.append(oversized)
            incompatible = json.loads(first_bytes)
            incompatible["minimum_bootstrap_protocol"] = cli.SUPPORT_BOOTSTRAP_PROTOCOL + 1
            malformed_manifests.append(incompatible)
            for index, value in enumerate(malformed_manifests):
                with self.subTest(manifest_rejection=index), self.assertRaises(cli.DesktopCLIError):
                    cli.parse_support_manifest(
                        (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()
                    )
            duplicate_json = first_bytes.replace(
                b'{"bootstrap_protocol":1,',
                b'{"bootstrap_protocol":1,"bootstrap_protocol":1,',
                1,
            )
            with self.assertRaises(cli.DesktopCLIError):
                cli.parse_support_manifest(duplicate_json)

            for mutation in (
                "unknown-file", "mode", "hash", "size", "symlink",
                "hardlink", "fifo", "directory", "manifest-mode",
            ):
                with self.subTest(filesystem_rejection=mutation):
                    case_root = root / f"case-{mutation}"
                    case = publish_fixture(case_root)
                    target = case / "desktop_user_cli.py"
                    if mutation == "unknown-file":
                        (case / "unknown.py").write_text("foreign", encoding="utf-8")
                        (case / "unknown.py").chmod(0o600)
                    elif mutation == "mode":
                        target.chmod(0o644)
                    elif mutation == "hash":
                        original = target.read_bytes()
                        target.write_bytes(b"X" + original[1:])
                        target.chmod(0o600)
                    elif mutation == "size":
                        target.write_bytes(target.read_bytes() + b"X")
                        target.chmod(0o600)
                    elif mutation == "symlink":
                        target.unlink()
                        target.symlink_to(root / "outside-support-payload")
                    elif mutation == "hardlink":
                        original = target.read_bytes()
                        target.unlink()
                        outside = root / "outside-support-payload"
                        outside.write_bytes(original)
                        outside.chmod(0o600)
                        os.link(outside, target)
                    elif mutation == "fifo":
                        target.unlink()
                        os.mkfifo(target, 0o600)
                    elif mutation == "directory":
                        target.unlink()
                        target.mkdir(mode=0o700)
                    else:
                        (case / "support-manifest.json").chmod(0o644)
                    before = tree_snapshot(case_root)
                    with self.assertRaises(cli.DesktopCLIError):
                        cli.validate_support_generation(case)
                    self.assertEqual(tree_snapshot(case_root), before)

            state = cli.SupportState(
                high_water_version=parsed.support_version,
                high_water_manifest_sha256=parsed.manifest_sha256,
                last_good_generation=parsed.generation_id,
                failed_generations=(),
            )
            state_bytes = cli.support_state_bytes(state)
            self.assertEqual(cli.parse_support_state(state_bytes), state)
            state_unknown = json.loads(state_bytes)
            state_unknown["unknown"] = 1
            with self.assertRaises(cli.DesktopCLIError):
                cli.parse_support_state(
                    (json.dumps(state_unknown, sort_keys=True, separators=(",", ":")) + "\n").encode()
                )

            lower = cli.validate_support_generation(publish_fixture(root / "lower", "1.1.9"))
            same_substitution = cli.validate_support_generation(
                publish_fixture(root / "same", "1.2.0", b"# substituted\n")
            )
            failed = cli.validate_support_generation(publish_fixture(root / "failed", "1.3.0"))
            with self.assertRaises(cli.DesktopCLIError):
                cli.validate_support_candidate(lower.manifest, state)
            with self.assertRaises(cli.DesktopCLIError):
                cli.validate_support_candidate(same_substitution.manifest, state)
            failed_state = dataclasses.replace(
                state,
                failed_generations=(cli.FailedSupportGeneration(
                    failed.manifest.generation_id,
                    failed.manifest.manifest_sha256,
                ),),
            )
            with self.assertRaises(cli.DesktopCLIError):
                cli.validate_support_candidate(failed.manifest, failed_state)

            pending = cli.SupportPending(
                from_generation=parsed.generation_id,
                to_generation=failed.manifest.generation_id,
                to_manifest_sha256=failed.manifest.manifest_sha256,
                expected_current_dev=1,
                expected_current_ino=2,
                requested_argv_sha256="a" * 64,
            )
            pending_bytes = cli.support_pending_bytes(pending)
            self.assertEqual(cli.parse_support_pending(pending_bytes), pending)
            pending_unknown = json.loads(pending_bytes)
            pending_unknown["command"] = "must-not-be-stored"
            with self.assertRaises(cli.DesktopCLIError):
                cli.parse_support_pending(
                    (json.dumps(pending_unknown, sort_keys=True, separators=(",", ":")) + "\n").encode()
                )

    def test_support_bootstrap_local_transaction_recovers_rolls_back_and_preserves_app_plane(self) -> None:
        import importlib

        bootstrap = importlib.import_module("scripts.support_bootstrap")

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            archive, app_manifest = write_artifacts(root, "0.1.8")
            platform = FakePlatform()
            cli.install(
                archive, app_manifest, home=home, platform=platform,
                effective_uid=501,
            )
            paths = cli._paths(home)
            support = paths.root / "support"
            self.assertFalse((paths.root / "cli").exists())
            self.assertEqual(
                {item.name for item in support.iterdir()},
                {"current", "state.json", "versions"},
            )
            initial_target = os.readlink(support / "current")
            self.assertRegex(initial_target, r"^versions/0\.1\.8-[0-9a-f]{12}$")
            initial_generation = initial_target.removeprefix("versions/")
            self.assertEqual(
                {item.name for item in (support / initial_target).iterdir()},
                {"support-manifest.json", "desktop_user_cli.py", "verify-app-archive.py"},
            )
            self.assertEqual(stat.S_IMODE(paths.launcher.stat().st_mode), 0o755)
            self.assertIn(b"lingtai.desktop.support/v1", paths.launcher.read_bytes())

            delegated: list[tuple[Path, list[str]]] = []
            with mock.patch.dict(os.environ, {
                cli.SUPPORT_REEXEC_MARKER: "1", "SUPPORT_SENTINEL": "preserved",
            }, clear=False):
                self.assertEqual(bootstrap.run_launcher(
                    ["version"], home=home,
                    installed_runner=lambda module, arguments: (
                        delegated.append((module, list(arguments))) or 17
                    ),
                ), 17)
                self.assertNotIn(cli.SUPPORT_REEXEC_MARKER, os.environ)
                self.assertEqual(os.environ["SUPPORT_SENTINEL"], "preserved")
            self.assertEqual(delegated[0][0].parent.name, initial_generation)
            self.assertEqual(delegated[0][1], ["version"])

            def app_plane_facts() -> tuple[tuple[str, str, int, int, int, bytes], ...]:
                result: list[tuple[str, str, int, int, int, bytes]] = []
                owned = [paths.versions, paths.receipts]
                if paths.current.exists() or paths.current.is_symlink():
                    owned.append(paths.current)
                for owned_root in owned:
                    candidates = [owned_root]
                    if owned_root.is_dir() and not owned_root.is_symlink():
                        candidates.extend(owned_root.rglob("*"))
                    for path in sorted(candidates, key=os.fspath):
                        facts = path.lstat()
                        relative = os.fspath(path.relative_to(paths.root))
                        if path.is_symlink():
                            kind, content = "symlink", os.fsencode(os.readlink(path))
                        elif path.is_dir():
                            kind, content = "directory", b""
                        elif path.is_file():
                            kind, content = "file", path.read_bytes()
                        else:
                            kind, content = "other", b""
                        result.append((
                            relative, kind, stat.S_IMODE(facts.st_mode),
                            facts.st_dev, facts.st_ino, content,
                        ))
                return tuple(result)

            app_before = app_plane_facts()
            module_source = Path(cli.__file__).read_bytes()
            verifier_source = (Path(cli.__file__).parent / "verify-app-archive.py").read_bytes()
            next_module = module_source + b"\nSUPPORT_FIXTURE_GENERATION = 'next'\n"
            next_verifier = verifier_source + b"\n# support fixture next\n"
            fixture = root / "support-fixture"
            fixture.mkdir()
            module_path = fixture / "desktop_user_cli.py"
            verifier_path = fixture / "verify-app-archive.py"
            module_path.write_bytes(next_module)
            verifier_path.write_bytes(next_verifier)
            module_path.chmod(0o600)
            verifier_path.chmod(0o600)
            argv = [os.fspath(paths.launcher), "doctor"]
            environment = {"HOME": os.fspath(home), "SENTINEL": "preserved"}
            exec_calls: list[tuple[Path, list[str], dict[str, str], Path]] = []
            original_cwd = Path.cwd()

            staged = cli.stage_local_support_update(
                module_path, verifier_path,
                support_version="0.1.9", release_tag="v0.1.9",
                argv=argv, environment=environment, home=home,
                exec_launcher=lambda launcher, live_argv, live_environment: exec_calls.append((
                    launcher, list(live_argv), dict(live_environment), Path.cwd(),
                )),
            )
            self.assertEqual(os.readlink(support / "current"), initial_target)
            self.assertTrue((support / "pending.json").is_file())
            self.assertTrue((support / "versions" / staged).is_dir())
            self.assertEqual(exec_calls[0][0], paths.launcher)
            self.assertEqual(exec_calls[0][1], argv)
            self.assertEqual(exec_calls[0][2]["SENTINEL"], "preserved")
            self.assertEqual(
                exec_calls[0][2][cli.SUPPORT_REEXEC_MARKER], "1",
            )
            self.assertEqual(exec_calls[0][3], original_cwd)
            self.assertEqual(app_plane_facts(), app_before)
            before_invalid = tree_snapshot(support)
            with self.assertRaises(bootstrap.BootstrapError):
                bootstrap.run_launcher(
                    ["doctor", "unexpected"], home=home,
                    installed_runner=lambda *_: 0,
                )
            self.assertEqual(tree_snapshot(support), before_invalid)

            selected = bootstrap.process_pending(home=home)
            self.assertEqual(selected.generation_id, staged)
            self.assertEqual(os.readlink(support / "current"), f"versions/{staged}")
            self.assertFalse((support / "pending.json").exists())
            state = cli.parse_support_state((support / "state.json").read_bytes())
            self.assertEqual(state.high_water_version, "0.1.9")
            self.assertEqual(state.last_good_generation, staged)
            self.assertEqual(app_plane_facts(), app_before)

            doctor_output: list[str] = []
            cli.run_installed(
                ["doctor"], home=home, platform=platform,
                transport=unavailable_release_transport(), tty=lambda: False,
                output=doctor_output.append,
            )
            joined = "\n".join(doctor_output)
            self.assertIn("bootstrap protocol: 1", joined)
            self.assertIn(f"support generation: {staged}", joined)
            self.assertIn("support high-water: 0.1.9", joined)
            self.assertIn("support pending: none", joined)

            def stage_version(version: str, suffix: bytes) -> str:
                module_path.write_bytes(module_source + suffix)
                verifier_path.write_bytes(verifier_source + suffix)
                module_path.chmod(0o600)
                verifier_path.chmod(0o600)
                return cli.stage_local_support_update(
                    module_path, verifier_path,
                    support_version=version, release_tag=f"v{version}",
                    argv=argv, environment=environment, home=home,
                    exec_launcher=lambda *_: None,
                )

            failed_target = stage_version("0.1.10", b"\n# import failure fixture\n")
            selected = bootstrap.process_pending(
                home=home,
                self_test_runner=lambda generation: (_ for _ in ()).throw(
                    bootstrap.BootstrapError("injected support self-test failure")
                ),
            )
            self.assertEqual(selected.generation_id, staged)
            self.assertEqual(os.readlink(support / "current"), f"versions/{staged}")
            failed_state = cli.parse_support_state((support / "state.json").read_bytes())
            self.assertIn(failed_target, [item.generation_id for item in failed_state.failed_generations])
            self.assertEqual(failed_state.high_water_version, "0.1.9")
            self.assertEqual(app_plane_facts(), app_before)

            rollback_target = stage_version("0.1.11", b"\n# rollback state crash fixture\n")
            bootstrap._FAILPOINT = "rollback-state-published"
            try:
                with self.assertRaises(bootstrap.InjectedCrash):
                    bootstrap.process_pending(
                        home=home,
                        self_test_runner=lambda generation: (_ for _ in ()).throw(
                            bootstrap.BootstrapError("injected support self-test failure")
                        ),
                    )
            finally:
                bootstrap._FAILPOINT = None
            self.assertEqual(os.readlink(support / "current"), f"versions/{staged}")
            self.assertTrue((support / "pending.json").exists())
            rollback_state = cli.parse_support_state((support / "state.json").read_bytes())
            self.assertIn(
                rollback_target,
                [item.generation_id for item in rollback_state.failed_generations],
            )
            recovered = bootstrap.process_pending(
                home=home,
                self_test_runner=lambda generation: (_ for _ in ()).throw(
                    AssertionError("recorded failed target was retried")
                ),
            )
            self.assertEqual(recovered.generation_id, staged)
            self.assertFalse((support / "pending.json").exists())

            crash_target = stage_version("0.1.12", b"\n# pointer crash fixture\n")
            bootstrap._FAILPOINT = "pointer-replaced"
            try:
                with self.assertRaises(bootstrap.InjectedCrash):
                    bootstrap.process_pending(
                        home=home, self_test_runner=lambda generation: None,
                    )
            finally:
                bootstrap._FAILPOINT = None
            self.assertEqual(os.readlink(support / "current"), f"versions/{crash_target}")
            self.assertTrue((support / "pending.json").exists())
            recovered = bootstrap.process_pending(
                home=home, self_test_runner=lambda generation: None,
            )
            self.assertEqual(recovered.generation_id, crash_target)
            self.assertFalse((support / "pending.json").exists())
            self.assertEqual(app_plane_facts(), app_before)

            state_target = stage_version("0.1.13", b"\n# state crash fixture\n")
            bootstrap._FAILPOINT = "state-published"
            try:
                with self.assertRaises(bootstrap.InjectedCrash):
                    bootstrap.process_pending(
                        home=home, self_test_runner=lambda generation: None,
                    )
            finally:
                bootstrap._FAILPOINT = None
            self.assertEqual(os.readlink(support / "current"), f"versions/{state_target}")
            self.assertTrue((support / "pending.json").exists())
            recovered = bootstrap.process_pending(
                home=home, self_test_runner=lambda generation: None,
            )
            self.assertEqual(recovered.generation_id, state_target)
            self.assertFalse((support / "pending.json").exists())
            self.assertEqual(app_plane_facts(), app_before)

            removal_target = stage_version("0.1.14", b"\n# pending removal crash fixture\n")
            bootstrap._FAILPOINT = "pending-removed"
            try:
                with self.assertRaises(bootstrap.InjectedCrash):
                    bootstrap.process_pending(
                        home=home, self_test_runner=lambda generation: None,
                    )
            finally:
                bootstrap._FAILPOINT = None
            self.assertEqual(os.readlink(support / "current"), f"versions/{removal_target}")
            self.assertFalse((support / "pending.json").exists())
            recovered = bootstrap.process_pending(
                home=home, self_test_runner=lambda generation: None,
            )
            self.assertEqual(recovered.generation_id, removal_target)
            self.assertEqual(app_plane_facts(), app_before)

            support_before_app_uninstall = tree_snapshot(support)
            launcher_before = paths.launcher.read_bytes()
            cli.uninstall_version("0.1.8", home=home, effective_uid=501)
            self.assertEqual(tree_snapshot(support), support_before_app_uninstall)
            self.assertEqual(paths.launcher.read_bytes(), launcher_before)

            foreign = support / "KEEP"
            foreign.write_text("foreign", encoding="utf-8")
            before_refusal = tree_snapshot(home)
            with self.assertRaises(cli.DesktopCLIError):
                cli.uninstall_all(home=home, effective_uid=501)
            self.assertEqual(tree_snapshot(home), before_refusal)
            foreign.unlink()
            cli.uninstall_all(home=home, effective_uid=501)
            self.assertFalse(paths.root.exists())
            self.assertFalse(paths.launcher.exists())

    def test_production_source_contains_no_quarantine_bypass(self) -> None:
        source = (Path(__file__).parents[1] / "scripts/desktop_user_cli.py").read_text()
        forbidden = ("xattr -d", "spctl --master-disable", "--no-quarantine")
        self.assertFalse(any(value in source for value in forbidden))

    def test_command_errors_version_collision_and_unknown_file_uninstall_refusal(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            with self.assertRaisesRegex(cli.DesktopCLIError, "no current"):
                cli.run_installed([], home=home, platform=FakePlatform())
            with self.assertRaisesRegex(cli.DesktopCLIError, "unknown command"):
                cli.run_installed(["surprise"], home=home, platform=FakePlatform())

            archive, manifest = write_artifacts(root)
            platform = FakePlatform()
            cli.install(archive, manifest, home=home, platform=platform, effective_uid=501)
            managed = home / ".local/share/lingtai-desktop"
            unknown = managed / "receipts/README"
            unknown.write_text("not owned")
            with self.assertRaisesRegex(cli.DesktopCLIError, "unknown files"):
                cli.uninstall_all(home=home, effective_uid=501)
            self.assertEqual(os.readlink(managed / "current"), "versions/0.1.5")
            self.assertTrue((managed / "versions/0.1.5/LingTai.app").is_dir())

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            archive, manifest = write_artifacts(root)
            collision = home / ".local/share/lingtai-desktop/versions/0.1.5"
            collision.mkdir(parents=True)
            with self.assertRaisesRegex(cli.DesktopCLIError, "version collision"):
                cli.install(archive, manifest, home=home,
                            platform=FakePlatform(), effective_uid=501)


if __name__ == "__main__":
    unittest.main()
