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


def identity_tree_snapshot(
        root: Path) -> tuple[tuple[str, str, int, int, int, bytes], ...]:
    result: list[tuple[str, str, int, int, int, bytes]] = []
    if not root.exists() and not root.is_symlink():
        return tuple()
    candidates = [root]
    if root.is_dir() and not root.is_symlink():
        candidates.extend(root.rglob("*"))
    for path in sorted(candidates, key=os.fspath):
        relative = "." if path == root else os.fspath(path.relative_to(root))
        facts = path.lstat()
        if path.is_symlink():
            kind, content = "symlink", os.fsencode(os.readlink(path))
        elif path.is_dir():
            kind, content = "directory", b""
        elif path.is_file():
            kind, content = "file", path.read_bytes()
        else:
            kind, content = "other", b""
        result.append((
            relative, kind, stat.S_IMODE(facts.st_mode), facts.st_dev, facts.st_ino,
            content,
        ))
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
                    arguments=["open"], home=home, platform=platform, transport=transport,
                    skip_support_check=True,
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
                    arguments=["update"], home=home, platform=platform,
                    skip_support_check=True,
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
                    arguments=["update"], home=home, platform=platform,
                    skip_support_check=True,
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
                arguments=["version"], home=home, platform=platform,
                skip_support_check=True,
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
                        arguments=arguments, home=home, platform=platform,
                        skip_support_check=True,
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
                        arguments=["version"], home=home, platform=platform,
                        skip_support_check=True,
                        transport=FakeReleaseTransport({}), output=lambda _: None,
                        clock=lambda: 1_001.0, tty=lambda: True, prompt=decline,
                    )
                    self.assertEqual(len(prompts), 1)
                    self.assertEqual(tree_snapshot(managed), before_declines)

            confirmed_transport = official_release_transport(new_archive, new_manifest, "0.1.6")
            confirmed_output: list[str] = []
            cli.run_installed(
                arguments=["version"], home=home, platform=platform,
                skip_support_check=True,
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
                arguments=["update"], home=explicit_home, platform=explicit_platform,
                skip_support_check=True,
                transport=official_release_transport(new_archive, new_manifest, "0.1.6"),
                output=lambda _: None, clock=lambda: 2_000.0, tty=lambda: True,
                prompt=lambda _: (_ for _ in ()).throw(AssertionError("explicit update prompted")),
                effective_uid=501,
            )
            explicit_cache = explicit_home / ".local/share/lingtai-desktop/update-check.json"
            self.assertEqual(json.loads(explicit_cache.read_text())["checked_at"], 2000)
            cli.run_installed(
                arguments=["uninstall", "--version", "0.1.5"], home=explicit_home,
                skip_support_check=True,
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
                arguments=["version"], home=failure_home, platform=FakePlatform(),
                skip_support_check=True,
                transport=initial_check, output=lambda _: None,
                clock=lambda: 3_000.0, tty=lambda: False,
            )
            failure_managed = failure_home / ".local/share/lingtai-desktop"
            failure_before = tree_snapshot(failure_managed)
            failing_platform = FakePlatform()
            failing_platform.fail = "verifier"
            failure_output: list[str] = []
            cli.run_installed(
                arguments=["open"], home=failure_home, platform=failing_platform,
                skip_support_check=True,
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
                        arguments=["version"], home=failure_home, platform=FakePlatform(),
                        skip_support_check=True,
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
                    arguments=["version"], home=home, platform=FakePlatform(),
                    skip_support_check=True,
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
                        arguments=["version"], home=home, platform=FakePlatform(),
                        skip_support_check=True,
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
            if managed.exists():
                shutil.rmtree(managed)
            managed.parent.mkdir(parents=True, exist_ok=True)
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
                explicit_retry=False,
                rollback_pointer_name=".rollback-" + "b" * 32,
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

            selected = bootstrap.process_pending(
                home=home, invocation_argv=argv,
            )
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
                invocation_argv=argv,
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
                        invocation_argv=argv,
                        self_test_runner=lambda generation: (_ for _ in ()).throw(
                            bootstrap.BootstrapError("injected support self-test failure")
                        ),
                    )
            finally:
                bootstrap._FAILPOINT = None
            self.assertEqual(
                os.readlink(support / "current"), f"versions/{rollback_target}"
            )
            self.assertTrue((support / "pending.json").exists())
            rollback_state = cli.parse_support_state((support / "state.json").read_bytes())
            self.assertIn(
                rollback_target,
                [item.generation_id for item in rollback_state.failed_generations],
            )
            recovered = bootstrap.process_pending(
                home=home,
                invocation_argv=argv,
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
                        home=home,
                        invocation_argv=argv, self_test_runner=lambda generation: None,
                    )
            finally:
                bootstrap._FAILPOINT = None
            self.assertEqual(os.readlink(support / "current"), f"versions/{crash_target}")
            self.assertTrue((support / "pending.json").exists())
            recovered = bootstrap.process_pending(
                home=home,
                invocation_argv=argv, self_test_runner=lambda generation: None,
            )
            self.assertEqual(recovered.generation_id, crash_target)
            self.assertFalse((support / "pending.json").exists())
            self.assertEqual(app_plane_facts(), app_before)

            state_target = stage_version("0.1.13", b"\n# state crash fixture\n")
            bootstrap._FAILPOINT = "state-published"
            try:
                with self.assertRaises(bootstrap.InjectedCrash):
                    bootstrap.process_pending(
                        home=home,
                        invocation_argv=argv, self_test_runner=lambda generation: None,
                    )
            finally:
                bootstrap._FAILPOINT = None
            self.assertEqual(os.readlink(support / "current"), f"versions/{state_target}")
            self.assertTrue((support / "pending.json").exists())
            recovered = bootstrap.process_pending(
                home=home,
                invocation_argv=argv, self_test_runner=lambda generation: None,
            )
            self.assertEqual(recovered.generation_id, state_target)
            self.assertFalse((support / "pending.json").exists())
            self.assertEqual(app_plane_facts(), app_before)

            removal_target = stage_version("0.1.14", b"\n# pending removal crash fixture\n")
            bootstrap._FAILPOINT = "pending-removed"
            try:
                with self.assertRaises(bootstrap.InjectedCrash):
                    bootstrap.process_pending(
                        home=home,
                        invocation_argv=argv, self_test_runner=lambda generation: None,
                    )
            finally:
                bootstrap._FAILPOINT = None
            self.assertEqual(os.readlink(support / "current"), f"versions/{removal_target}")
            self.assertFalse((support / "pending.json").exists())
            recovered = bootstrap.process_pending(
                home=home,
                invocation_argv=argv, self_test_runner=lambda generation: None,
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

    def test_review_high1_fresh_failure_retry_is_complete_and_offline_usable(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            shared = (home / ".local", home / ".local/bin", home / ".local/share")
            for directory in shared:
                directory.mkdir(parents=True, exist_ok=True)
                directory.chmod(0o755)
            foreign = home / ".local/share/FOREIGN"
            foreign.write_text("preserve", encoding="utf-8")
            foreign_identity = (foreign.lstat().st_dev, foreign.lstat().st_ino)
            archive, manifest = write_artifacts(root)
            with mock.patch.object(cli, "_FAILPOINT", "receipt"):
                with self.assertRaisesRegex(cli.DesktopCLIError, "receipt"):
                    cli.install(
                        archive, manifest, home=home, platform=FakePlatform(),
                        effective_uid=501,
                    )
            cli.install(
                archive, manifest, home=home, platform=FakePlatform(), effective_uid=501,
            )
            self.assertEqual(
                (foreign.lstat().st_dev, foreign.lstat().st_ino), foreign_identity,
            )
            self.assertEqual(foreign.read_text(encoding="utf-8"), "preserve")
            managed = home / ".local/share/lingtai-desktop"
            cache = managed / "update-check.json"
            cache.write_bytes(cli._update_cache_bytes("0.1.5", 4_000_000_000))
            cache.chmod(0o600)
            result = cli.subprocess.run(
                [home / ".local/bin/lingtai-desktop", "version"],
                env={
                    "HOME": os.fspath(home),
                    "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
                    "LANG": "C", "LC_ALL": "C",
                },
                stdout=cli.subprocess.PIPE, stderr=cli.subprocess.PIPE,
                text=True, timeout=10, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("version: 0.1.5", result.stdout)

    def test_review_high1_all_ordinary_fresh_boundaries_clean_then_retry(self) -> None:
        failpoints = (
            "extraction", "receipt", "support-generation-published",
            "support-state-published", "support-current-published", "launcher",
            "launcher-post-visible", "support-validation", "current",
            "app-current-post-visible",
        )
        for failpoint in failpoints:
            with self.subTest(failpoint=failpoint), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                home = root / "home"
                home.mkdir()
                shared = (home / ".local", home / ".local/bin", home / ".local/share")
                for directory in shared:
                    directory.mkdir(parents=True, exist_ok=True)
                    directory.chmod(0o755)
                shared_identities = {
                    path: (path.lstat().st_dev, path.lstat().st_ino) for path in shared
                }
                foreign = home / ".local/share/FOREIGN"
                foreign.write_bytes(b"foreign")
                foreign_identity = (foreign.lstat().st_dev, foreign.lstat().st_ino)
                archive, manifest = write_artifacts(root)
                with mock.patch.object(cli, "_FAILPOINT", failpoint):
                    with self.assertRaises(cli.DesktopCLIError):
                        cli.install(
                            archive, manifest, home=home, platform=FakePlatform(),
                            effective_uid=501,
                        )
                self.assertFalse((home / ".local/share/lingtai-desktop").exists())
                self.assertFalse((home / ".local/bin/lingtai-desktop").exists())
                self.assertEqual(
                    {path: (path.lstat().st_dev, path.lstat().st_ino) for path in shared},
                    shared_identities,
                )
                self.assertEqual(
                    (foreign.lstat().st_dev, foreign.lstat().st_ino), foreign_identity,
                )
                cli.install(
                    archive, manifest, home=home, platform=FakePlatform(),
                    effective_uid=501,
                )
                self.assertEqual(cli._active(cli._paths(home))[0], "0.1.5")
                cli._validate_owned_cli(cli._paths(home))

    def test_review_high1_parent_fsync_failures_clean_visible_identity_then_retry(self) -> None:
        for parent_name in ("lingtai-desktop", "support"):
            with self.subTest(parent=parent_name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                home = root / "home"
                home.mkdir()
                archive, manifest = write_artifacts(root)
                original = cli._fsync_directory
                injected = [False]

                def fail_once(path: Path) -> None:
                    if path.name == parent_name and not injected[0]:
                        injected[0] = True
                        raise cli.DesktopCLIError("injected parent fsync failure")
                    original(path)

                with mock.patch.object(cli, "_fsync_directory", side_effect=fail_once):
                    with self.assertRaisesRegex(cli.DesktopCLIError, "parent fsync"):
                        cli.install(
                            archive, manifest, home=home, platform=FakePlatform(),
                            effective_uid=501,
                        )
                self.assertTrue(injected[0])
                self.assertFalse((home / ".local/share/lingtai-desktop").exists())
                self.assertFalse((home / ".local/bin/lingtai-desktop").exists())
                cli.install(
                    archive, manifest, home=home, platform=FakePlatform(),
                    effective_uid=501,
                )
                cli._validate_owned_cli(cli._paths(home))

    def test_review_high1_crash_journal_resumes_every_publication_boundary(self) -> None:
        boundaries = (
            "journal", "extraction", "receipt", "version",
            "support-generation", "support-state", "support-current",
            "support-launcher", "support-validation", "support", "app-current",
        )
        for boundary in boundaries:
            with self.subTest(boundary=boundary), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                home = root / "home"
                home.mkdir()
                archive, manifest = write_artifacts(root)
                with mock.patch.object(cli, "_FAILPOINT", f"crash:{boundary}"):
                    with self.assertRaises(cli.InjectedInitialInstallCrash):
                        cli.install(
                            archive, manifest, home=home, platform=FakePlatform(),
                            effective_uid=501,
                        )
                journal = home / ".local/share/lingtai-desktop/initial-install.json"
                self.assertTrue(journal.is_file())
                cli.install(
                    archive, manifest, home=home, platform=FakePlatform(),
                    effective_uid=501,
                )
                paths = cli._paths(home)
                self.assertFalse(journal.exists())
                self.assertEqual(cli._active(cli._paths(home))[0], "0.1.5")
                cli._validate_owned_cli(paths)
                result = cli.subprocess.run(
                    [paths.launcher, "version"],
                    env={"HOME": os.fspath(home), "PATH": "/usr/bin:/bin",
                         "LANG": "C", "LC_ALL": "C"},
                    stdout=cli.subprocess.PIPE, stderr=cli.subprocess.PIPE,
                    text=True, timeout=10, check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("version: 0.1.5", result.stdout)

    def test_review_high1_crash_retry_preserves_racer_replacement(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            archive, manifest = write_artifacts(root)
            with mock.patch.object(cli, "_FAILPOINT", "crash:support-current"):
                with self.assertRaises(cli.InjectedInitialInstallCrash):
                    cli.install(
                        archive, manifest, home=home, platform=FakePlatform(),
                        effective_uid=501,
                    )
            paths = cli._paths(home)
            paths.support_current.unlink()
            paths.support_current.symlink_to("versions/foreign-racer")
            identity = (paths.support_current.lstat().st_dev,
                        paths.support_current.lstat().st_ino)
            with self.assertRaises(cli.DesktopCLIError):
                cli.install(
                    archive, manifest, home=home, platform=FakePlatform(),
                    effective_uid=501,
                )
            self.assertEqual(
                (paths.support_current.lstat().st_dev,
                 paths.support_current.lstat().st_ino), identity,
            )
            self.assertEqual(os.readlink(paths.support_current), "versions/foreign-racer")

    def test_review_high2_executes_validated_bytes_and_refuses_pointer_commit_race(self) -> None:
        import importlib

        bootstrap = importlib.import_module("scripts.support_bootstrap")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            versions = root / "support/versions"
            versions.mkdir(parents=True)
            payloads = {
                "desktop_user_cli.py": b"def installed_main(argv=None):\n    return 11\n",
                "verify-app-archive.py": b"# verifier\n",
            }
            manifest_bytes = cli.build_support_manifest_bytes("1.0.0", "v1.0.0", payloads)
            generation_id = json.loads(manifest_bytes)["generation_id"]
            generation_path = versions / generation_id
            generation_path.mkdir(mode=0o700)
            (generation_path / "support-manifest.json").write_bytes(manifest_bytes)
            for name, payload in payloads.items():
                (generation_path / name).write_bytes(payload)
            for child in generation_path.iterdir():
                child.chmod(0o600)
            paths = dataclasses.replace(
                bootstrap._paths(root),
                support=root / "support", versions=versions,
            )
            generation = bootstrap._validate_generation(paths, generation_id)
            injected_marker = root / "UNVALIDATED-EXECUTED"
            (generation_path / "desktop_user_cli.py").write_text(
                "from pathlib import Path\n"
                f"Path({os.fspath(injected_marker)!r}).write_text('executed')\n"
                "def installed_main(argv=None):\n    return 73\n",
                encoding="utf-8",
            )
            (generation_path / "desktop_user_cli.py").chmod(0o600)
            self.assertEqual(bootstrap._load_and_run(generation, ["version"]), 11)
            self.assertFalse(injected_marker.exists())

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            archive, manifest = write_artifacts(root)
            cli.install(archive, manifest, home=home,
                        platform=FakePlatform(), effective_uid=501)
            paths = cli._paths(home)
            support = paths.support
            source = os.readlink(paths.support_current).removeprefix("versions/")
            fixture = root / "fixture"
            fixture.mkdir()
            module = fixture / "desktop_user_cli.py"
            verifier = fixture / "verify-app-archive.py"
            module.write_bytes(Path(cli.__file__).read_bytes() + b"\n# target race\n")
            verifier.write_bytes(
                (Path(cli.__file__).parent / "verify-app-archive.py").read_bytes()
                + b"\n# target race\n"
            )
            module.chmod(0o600)
            verifier.chmod(0o600)
            target = cli.stage_local_support_update(
                module, verifier, support_version="0.1.6", release_tag="v0.1.6",
                argv=[os.fspath(paths.launcher), "version"], home=home,
                exec_launcher=lambda *_: None,
            )
            foreign_pointer: list[tuple[int, int]] = []

            def replace_current(_: object) -> None:
                paths.support_current.unlink()
                paths.support_current.symlink_to(f"versions/{source}")
                facts = paths.support_current.lstat()
                foreign_pointer.append((facts.st_dev, facts.st_ino))

            with self.assertRaises(bootstrap.BootstrapError):
                bootstrap.process_pending(
                    home=home,
                    invocation_argv=[os.fspath(paths.launcher), "version"],
                    self_test_runner=replace_current,
                )
            state = cli.parse_support_state(paths.support_state.read_bytes())
            self.assertNotEqual(state.last_good_generation, target)
            self.assertTrue(paths.support_pending.exists())
            self.assertEqual(os.readlink(paths.support_current), f"versions/{source}")
            self.assertEqual(
                (paths.support_current.lstat().st_dev, paths.support_current.lstat().st_ino),
                foreign_pointer[0],
            )

    def test_review_high2_all_generation_and_pointer_wedges_fail_closed(self) -> None:
        import importlib

        bootstrap = importlib.import_module("scripts.support_bootstrap")
        for phase in ("before-self-test", "after-self-test"):
            for wedge in ("payload", "manifest", "generation", "current"):
                with (self.subTest(phase=phase, wedge=wedge),
                      tempfile.TemporaryDirectory() as temporary):
                    root = Path(temporary)
                    home = root / "home"
                    home.mkdir()
                    archive, manifest = write_artifacts(root)
                    cli.install(
                        archive, manifest, home=home, platform=FakePlatform(),
                        effective_uid=501,
                    )
                    paths = cli._paths(home)
                    source = os.readlink(paths.support_current).removeprefix("versions/")
                    app_before = tuple(identity_tree_snapshot(path) for path in (
                        paths.versions, paths.receipts, paths.current, paths.update_cache,
                    ))
                    fixture = root / "fixture"
                    fixture.mkdir()
                    module = fixture / "desktop_user_cli.py"
                    verifier = fixture / "verify-app-archive.py"
                    module.write_bytes(Path(cli.__file__).read_bytes() + b"\n# wedge target\n")
                    verifier.write_bytes(
                        (Path(cli.__file__).parent / "verify-app-archive.py").read_bytes()
                        + b"\n# wedge target\n"
                    )
                    module.chmod(0o600)
                    verifier.chmod(0o600)
                    target = cli.stage_local_support_update(
                        module, verifier, support_version="0.1.6", release_tag="v0.1.6",
                        argv=[os.fspath(paths.launcher), "version"], home=home,
                        exec_launcher=lambda *_: None,
                    )
                    target_path = paths.support_versions / target
                    marker = root / "UNMANIFESTED-MARKER"
                    foreign_identity: list[tuple[int, int]] = []

                    def mutate() -> None:
                        if wedge == "payload":
                            payload = target_path / "desktop_user_cli.py"
                            payload.write_text(
                                "from pathlib import Path\n"
                                f"Path({os.fspath(marker)!r}).write_text('executed')\n"
                                "def support_self_test(): return True\n",
                                encoding="utf-8",
                            )
                            payload.chmod(0o600)
                        elif wedge == "manifest":
                            changed = target_path / "support-manifest.json"
                            changed.write_bytes(b"{}\n")
                            changed.chmod(0o600)
                        elif wedge == "generation":
                            saved = root / "validated-generation"
                            target_path.rename(saved)
                            shutil.copytree(saved, target_path)
                            foreign_identity.append((
                                target_path.lstat().st_dev, target_path.lstat().st_ino,
                            ))
                        else:
                            paths.support_current.unlink()
                            paths.support_current.symlink_to(f"versions/{source}")
                            foreign_identity.append((
                                paths.support_current.lstat().st_dev,
                                paths.support_current.lstat().st_ino,
                            ))

                    def runner(generation: object) -> None:
                        if phase == "before-self-test":
                            mutate()
                        bootstrap._production_self_test(generation)
                        if phase == "after-self-test":
                            mutate()

                    if wedge == "current":
                        with self.assertRaises(bootstrap.BootstrapError):
                            bootstrap.process_pending(
                                home=home,
                                invocation_argv=[
                                    os.fspath(paths.launcher), "version",
                                ],
                                self_test_runner=runner,
                            )
                    else:
                        selected = bootstrap.process_pending(
                            home=home,
                            invocation_argv=[
                                os.fspath(paths.launcher), "version",
                            ],
                            self_test_runner=runner,
                        )
                        self.assertEqual(selected.generation_id, source)
                    self.assertFalse(marker.exists())
                    state = cli.parse_support_state(paths.support_state.read_bytes())
                    self.assertEqual(state.last_good_generation, source)
                    self.assertEqual(paths.support_pending.exists(), wedge == "current")
                    self.assertEqual(
                        tuple(identity_tree_snapshot(path) for path in (
                            paths.versions, paths.receipts, paths.current,
                            paths.update_cache,
                        )), app_before,
                    )
                    if wedge == "generation":
                        self.assertEqual(
                            (target_path.lstat().st_dev, target_path.lstat().st_ino),
                            foreign_identity[0],
                        )
                    elif wedge == "current":
                        self.assertEqual(
                            (paths.support_current.lstat().st_dev,
                             paths.support_current.lstat().st_ino),
                            foreign_identity[0],
                        )

    def test_review_high2_active_import_retains_bytes_and_checks_current_identity(self) -> None:
        import importlib

        bootstrap = importlib.import_module("scripts.support_bootstrap")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            versions = root / "support/versions"
            versions.mkdir(parents=True)
            payloads = {
                "desktop_user_cli.py": b"def installed_main(argv=None):\n    return 19\n",
                "verify-app-archive.py": b"# verifier\n",
            }
            manifest_bytes = cli.build_support_manifest_bytes("1.0.0", "v1.0.0", payloads)
            generation_id = json.loads(manifest_bytes)["generation_id"]
            generation_path = versions / generation_id
            generation_path.mkdir(mode=0o700)
            (generation_path / "support-manifest.json").write_bytes(manifest_bytes)
            for name, payload in payloads.items():
                (generation_path / name).write_bytes(payload)
            for child in generation_path.iterdir():
                child.chmod(0o600)
            paths = dataclasses.replace(
                bootstrap._paths(root), support=root / "support", versions=versions,
            )
            generation = bootstrap._validate_generation(paths, generation_id)
            marker = root / "IMPORT-RACE-MARKER"
            payload_path = generation_path / "desktop_user_cli.py"
            payload_path.write_text(
                "from pathlib import Path\n"
                f"Path({os.fspath(marker)!r}).write_text('executed')\n"
                "def installed_main(argv=None): return 91\n",
                encoding="utf-8",
            )
            payload_path.chmod(0o600)
            self.assertEqual(bootstrap._load_and_run(generation, ["version"]), 19)
            self.assertFalse(marker.exists())

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            archive, manifest = write_artifacts(root)
            cli.install(
                archive, manifest, home=home, platform=FakePlatform(), effective_uid=501,
            )
            paths = cli._paths(home)
            source = os.readlink(paths.support_current)
            real_revalidate = bootstrap._revalidate_generation
            imported: list[bool] = []

            def race_after_validation(*args: object) -> object:
                generation = real_revalidate(*args)
                paths.support_current.unlink()
                paths.support_current.symlink_to(source)
                return generation

            with mock.patch.object(
                    bootstrap, "_revalidate_generation", side_effect=race_after_validation):
                with self.assertRaises(bootstrap.BootstrapError):
                    bootstrap.run_launcher(
                        ["version"], home=home,
                        installed_runner=lambda *_: imported.append(True) or 0,
                    )
            self.assertEqual(imported, [])

    def test_review_high3_rollback_pointer_crash_replays_authenticated_source(self) -> None:
        import importlib

        bootstrap = importlib.import_module("scripts.support_bootstrap")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            archive, manifest = write_artifacts(root)
            cli.install(archive, manifest, home=home,
                        platform=FakePlatform(), effective_uid=501)
            paths = cli._paths(home)
            source = os.readlink(paths.support_current).removeprefix("versions/")
            source_pointer_identity = (
                paths.support_current.lstat().st_dev, paths.support_current.lstat().st_ino,
            )
            fixture = root / "fixture"
            fixture.mkdir()
            module = fixture / "desktop_user_cli.py"
            verifier = fixture / "verify-app-archive.py"
            module.write_bytes(Path(cli.__file__).read_bytes() + b"\n# fail target\n")
            verifier.write_bytes(
                (Path(cli.__file__).parent / "verify-app-archive.py").read_bytes()
                + b"\n# fail target\n"
            )
            module.chmod(0o600)
            verifier.chmod(0o600)
            target = cli.stage_local_support_update(
                module, verifier, support_version="0.1.6", release_tag="v0.1.6",
                argv=[os.fspath(paths.launcher), "version"], home=home,
                exec_launcher=lambda *_: None,
            )
            bootstrap._FAILPOINT = "rollback-pointer-replaced"
            try:
                with self.assertRaises(bootstrap.InjectedCrash):
                    bootstrap.process_pending(
                        home=home,
                        invocation_argv=[os.fspath(paths.launcher), "version"],
                        self_test_runner=lambda _: (_ for _ in ()).throw(
                            bootstrap.BootstrapError("candidate failed")
                        ),
                    )
            finally:
                bootstrap._FAILPOINT = None
            recovered = bootstrap.process_pending(
                home=home,
                invocation_argv=[os.fspath(paths.launcher), "version"],
                self_test_runner=lambda _: (_ for _ in ()).throw(
                    AssertionError("failed target retried")
                ),
            )
            self.assertEqual(recovered.generation_id, source)
            self.assertFalse(paths.support_pending.exists())
            self.assertEqual(
                (paths.support_current.lstat().st_dev, paths.support_current.lstat().st_ino),
                source_pointer_identity,
            )
            state = cli.parse_support_state(paths.support_state.read_bytes())
            self.assertIn(target, [item.generation_id for item in state.failed_generations])
            self.assertEqual(state.high_water_version, "0.1.5")

    def test_review_high3_every_rollback_durable_boundary_recovers_once(self) -> None:
        import importlib

        bootstrap = importlib.import_module("scripts.support_bootstrap")
        for boundary in (
                "rollback-state-published", "rollback-temporary",
                "rollback-pointer-replaced", "pending-removed"):
            with self.subTest(boundary=boundary), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                home = root / "home"
                home.mkdir()
                archive, manifest = write_artifacts(root)
                cli.install(
                    archive, manifest, home=home, platform=FakePlatform(),
                    effective_uid=501,
                )
                paths = cli._paths(home)
                source = os.readlink(paths.support_current).removeprefix("versions/")
                source_identity = (
                    paths.support_current.lstat().st_dev,
                    paths.support_current.lstat().st_ino,
                )
                app_before = tuple(identity_tree_snapshot(path) for path in (
                    paths.versions, paths.receipts, paths.current, paths.update_cache,
                ))
                fixture = root / "fixture"
                fixture.mkdir()
                module = fixture / "desktop_user_cli.py"
                verifier = fixture / "verify-app-archive.py"
                module.write_bytes(Path(cli.__file__).read_bytes() + b"\n# rollback replay\n")
                verifier.write_bytes(
                    (Path(cli.__file__).parent / "verify-app-archive.py").read_bytes()
                    + b"\n# rollback replay\n"
                )
                module.chmod(0o600)
                verifier.chmod(0o600)
                target = cli.stage_local_support_update(
                    module, verifier, support_version="0.1.6", release_tag="v0.1.6",
                    argv=[os.fspath(paths.launcher), "version"], home=home,
                    exec_launcher=lambda *_: None,
                )
                bootstrap._FAILPOINT = boundary
                try:
                    with self.assertRaises(bootstrap.InjectedCrash):
                        bootstrap.process_pending(
                            home=home,
                            invocation_argv=[os.fspath(paths.launcher), "version"],
                            self_test_runner=lambda _: (_ for _ in ()).throw(
                                bootstrap.BootstrapError("candidate rejected")
                            ),
                        )
                finally:
                    bootstrap._FAILPOINT = None
                delegated: list[tuple[Path, list[str]]] = []
                exit_code = bootstrap.run_launcher(
                    ["version"], home=home,
                    self_test_runner=lambda _: (_ for _ in ()).throw(
                        AssertionError("failed target was retried")
                    ),
                    installed_runner=lambda module_path, arguments: (
                        delegated.append((module_path, list(arguments))) or 23
                    ),
                )
                self.assertEqual(exit_code, 23)
                self.assertEqual(delegated, [(
                    paths.support_versions / source / "desktop_user_cli.py", ["version"],
                )])
                self.assertFalse(paths.support_pending.exists())
                self.assertEqual(os.readlink(paths.support_current), f"versions/{source}")
                self.assertEqual(
                    (paths.support_current.lstat().st_dev,
                     paths.support_current.lstat().st_ino), source_identity,
                )
                state = cli.parse_support_state(paths.support_state.read_bytes())
                self.assertEqual(state.last_good_generation, source)
                self.assertEqual(state.high_water_version, "0.1.5")
                self.assertEqual(state.high_water_manifest_sha256,
                                 cli.parse_support_manifest(
                                     (paths.support_versions / source /
                                      "support-manifest.json").read_bytes()
                                 ).manifest_sha256)
                self.assertIn(target, [item.generation_id for item in state.failed_generations])
                self.assertEqual(
                    tuple(identity_tree_snapshot(path) for path in (
                        paths.versions, paths.receipts, paths.current, paths.update_cache,
                    )),
                    app_before,
                )

    def test_review_high3_rejects_and_preserves_foreign_rollback_backup(self) -> None:
        import importlib

        bootstrap = importlib.import_module("scripts.support_bootstrap")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            archive, manifest = write_artifacts(root)
            cli.install(
                archive, manifest, home=home, platform=FakePlatform(), effective_uid=501,
            )
            paths = cli._paths(home)
            fixture = root / "fixture"
            fixture.mkdir()
            module = fixture / "desktop_user_cli.py"
            verifier = fixture / "verify-app-archive.py"
            module.write_bytes(Path(cli.__file__).read_bytes() + b"\n# backup race\n")
            verifier.write_bytes(
                (Path(cli.__file__).parent / "verify-app-archive.py").read_bytes()
                + b"\n# backup race\n"
            )
            module.chmod(0o600)
            verifier.chmod(0o600)
            cli.stage_local_support_update(
                module, verifier, support_version="0.1.6", release_tag="v0.1.6",
                argv=[os.fspath(paths.launcher), "version"], home=home,
                exec_launcher=lambda *_: None,
            )
            pending = cli.parse_support_pending(paths.support_pending.read_bytes())
            foreign = paths.support / pending.rollback_pointer_name
            foreign.symlink_to(f"versions/{pending.to_generation}")
            identity = (foreign.lstat().st_dev, foreign.lstat().st_ino)
            before = paths.support_state.read_bytes()
            with self.assertRaises(bootstrap.BootstrapError):
                bootstrap.process_pending(
                    home=home,
                    invocation_argv=[os.fspath(paths.launcher), "version"],
                    self_test_runner=lambda _: None,
                )
            self.assertEqual((foreign.lstat().st_dev, foreign.lstat().st_ino), identity)
            self.assertEqual(paths.support_state.read_bytes(), before)
            self.assertTrue(paths.support_pending.exists())

    def test_review_high4_production_self_test_enforces_no_mutation_or_escape(self) -> None:
        import importlib

        bootstrap = importlib.import_module("scripts.support_bootstrap")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            cwd = root / "candidate-cwd"
            cwd.mkdir()
            archive, manifest = write_artifacts(root)
            cli.install(archive, manifest, home=home,
                        platform=FakePlatform(), effective_uid=501)
            paths = cli._paths(home)
            source = os.readlink(paths.support_current).removeprefix("versions/")
            markers = {
                "app": paths.receipts / "SELFTEST-APP-MUTATION",
                "support": paths.support / "SELFTEST-SUPPORT-MUTATION",
                "home": home / "SELFTEST-HOME-MUTATION",
                "cwd": cwd / "SELFTEST-CWD-MUTATION",
                "process": root / "SELFTEST-PROCESS-MUTATION",
                "exec": root / "SELFTEST-EXEC-MUTATION",
            }
            fixture = root / "fixture"
            fixture.mkdir()
            module = fixture / "desktop_user_cli.py"
            verifier = fixture / "verify-app-archive.py"
            attempt_code = "\n".join([
                "",
                "def _review_attempt(action):",
                "    try:",
                "        action()",
                "    except BaseException:",
                "        pass",
                f"_review_attempt(lambda: Path({os.fspath(markers['app'])!r}).write_text('bad'))",
                f"_review_attempt(lambda: Path({os.fspath(markers['support'])!r}).write_text('bad'))",
                f"_review_attempt(lambda: Path({os.fspath(markers['home'])!r}).write_text('bad'))",
                f"_review_attempt(lambda: Path({os.fspath(markers['cwd'])!r}).write_text('bad'))",
                "import subprocess as _review_subprocess",
                f"_review_attempt(lambda: _review_subprocess.run(['/usr/bin/touch', {os.fspath(markers['process'])!r}], check=False))",
                "import os as _review_os",
                f"_review_attempt(lambda: _review_os.execv('/usr/bin/touch', ['touch', {os.fspath(markers['exec'])!r}]))",
                "import socket as _review_socket",
                "import sys as _review_sys",
                "_review_attempt(lambda: _review_socket.socket().connect(('127.0.0.1', 9)))",
                "_review_attempt(lambda: _review_sys.audit('socket.connect', object(), ('mocked.invalid', 443)))",
                "import ctypes as _review_ctypes",
                "_review_attempt(lambda: _review_ctypes.CDLL(None))",
                "",
            ]).encode()
            module.write_bytes(Path(cli.__file__).read_bytes() + attempt_code)
            verifier.write_bytes(
                (Path(cli.__file__).parent / "verify-app-archive.py").read_bytes()
                + b"\n# isolation target\n"
            )
            module.chmod(0o600)
            verifier.chmod(0o600)
            target = cli.stage_local_support_update(
                module, verifier, support_version="0.1.6", release_tag="v0.1.6",
                argv=[os.fspath(paths.launcher), "version"], home=home,
                exec_launcher=lambda *_: None,
            )
            old_cwd = Path.cwd()
            try:
                os.chdir(cwd)
                selected = bootstrap.process_pending(
                    home=home,
                    invocation_argv=[os.fspath(paths.launcher), "version"],
                )
            finally:
                os.chdir(old_cwd)
            self.assertEqual(selected.generation_id, source)
            self.assertTrue(all(not path.exists() for path in markers.values()))
            state = cli.parse_support_state(paths.support_state.read_bytes())
            self.assertIn(target, [item.generation_id for item in state.failed_generations])

    def test_review_high4_self_test_body_attempts_are_denied_and_rolled_back(self) -> None:
        import importlib

        bootstrap = importlib.import_module("scripts.support_bootstrap")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            cwd = root / "caller-cwd"
            cwd.mkdir()
            archive, manifest = write_artifacts(root)
            cli.install(
                archive, manifest, home=home, platform=FakePlatform(), effective_uid=501,
            )
            paths = cli._paths(home)
            source = os.readlink(paths.support_current).removeprefix("versions/")
            app_before = tuple(identity_tree_snapshot(path) for path in (
                paths.versions, paths.receipts, paths.current, paths.update_cache,
            ))
            markers = {
                "app": paths.receipts / "BODY-APP-MUTATION",
                "support": paths.support / "BODY-SUPPORT-MUTATION",
                "home": home / "BODY-HOME-MUTATION",
                "cwd": cwd / "BODY-CWD-MUTATION",
                "process": root / "BODY-PROCESS-MUTATION",
                "exec": root / "BODY-EXEC-MUTATION",
            }
            fixture = root / "fixture"
            fixture.mkdir()
            module = fixture / "desktop_user_cli.py"
            verifier = fixture / "verify-app-archive.py"
            body_code = "\n".join([
                "",
                "_review_original_self_test = support_self_test",
                "def support_self_test():",
                "    import subprocess as _review_subprocess",
                "    import socket as _review_socket",
                "    import ctypes as _review_ctypes",
                "    import os as _review_os",
                "    import sys as _review_sys",
                "    def attempt(action):",
                "        try:",
                "            action()",
                "        except BaseException:",
                "            pass",
                f"    attempt(lambda: Path({os.fspath(markers['app'])!r}).write_text('bad'))",
                f"    attempt(lambda: Path({os.fspath(markers['support'])!r}).write_text('bad'))",
                f"    attempt(lambda: Path({os.fspath(markers['home'])!r}).write_text('bad'))",
                f"    attempt(lambda: Path({os.fspath(markers['cwd'])!r}).write_text('bad'))",
                f"    attempt(lambda: _review_subprocess.run(['/usr/bin/touch', {os.fspath(markers['process'])!r}], check=False))",
                "    attempt(lambda: _review_os._exit(0))",
                f"    attempt(lambda: _review_os.execv('/usr/bin/touch', ['touch', {os.fspath(markers['exec'])!r}]))",
                "    attempt(lambda: _review_socket.socket().connect(('127.0.0.1', 9)))",
                "    attempt(lambda: _review_sys.audit('socket.connect', object(), ('mocked.invalid', 443)))",
                "    attempt(lambda: _review_ctypes.CDLL(None))",
                "    return _review_original_self_test()",
                "",
            ]).encode()
            module.write_bytes(Path(cli.__file__).read_bytes() + body_code)
            verifier.write_bytes(
                (Path(cli.__file__).parent / "verify-app-archive.py").read_bytes()
                + b"\n# body isolation target\n"
            )
            module.chmod(0o600)
            verifier.chmod(0o600)
            target = cli.stage_local_support_update(
                module, verifier, support_version="0.1.6", release_tag="v0.1.6",
                argv=[os.fspath(paths.launcher), "version"], home=home,
                exec_launcher=lambda *_: None,
            )
            old_cwd = Path.cwd()
            try:
                os.chdir(cwd)
                selected = bootstrap.process_pending(
                    home=home,
                    invocation_argv=[os.fspath(paths.launcher), "version"],
                )
            finally:
                os.chdir(old_cwd)
            self.assertEqual(selected.generation_id, source)
            self.assertTrue(all(not marker.exists() for marker in markers.values()))
            self.assertEqual(
                tuple(identity_tree_snapshot(path) for path in (
                    paths.versions, paths.receipts, paths.current, paths.update_cache,
                )), app_before,
            )
            state = cli.parse_support_state(paths.support_state.read_bytes())
            self.assertIn(target, [item.generation_id for item in state.failed_generations])
            self.assertFalse(paths.support_pending.exists())
            delegated: list[Path] = []
            self.assertEqual(bootstrap.run_launcher(
                ["version"], home=home,
                installed_runner=lambda module_path, _arguments: (
                    delegated.append(module_path) or 29
                ),
            ), 29)
            self.assertEqual(
                delegated,
                [paths.support_versions / source / "desktop_user_cli.py"],
            )

    def test_review_high4_timeout_exception_nontrue_wrong_path_and_minimal_env(self) -> None:
        import importlib

        bootstrap = importlib.import_module("scripts.support_bootstrap")

        def generation(root: Path, source: bytes) -> object:
            return bootstrap.Generation(
                root / "1.0.0-000000000000", "1.0.0", "1.0.0-000000000000",
                "0" * 64, (1, 1), (), b"manifest", (
                    ("desktop_user_cli.py", source),
                    ("verify-app-archive.py", b"# verifier\n"),
                ),
            )

        fixtures = {
            "exception": b"def support_self_test():\n    raise RuntimeError('no')\n",
            "non-true": b"def support_self_test():\n    return 1\n",
            "wrong-path": b"def support_self_test():\n    return __file__ == '/wrong/path.py'\n",
        }
        for label, source in fixtures.items():
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                with self.assertRaises(bootstrap.BootstrapError):
                    bootstrap._production_self_test(generation(Path(temporary), source))
        with tempfile.TemporaryDirectory() as temporary:
            source = b"import time\ndef support_self_test():\n    time.sleep(2)\n    return True\n"
            with mock.patch.object(bootstrap, "SUPPORT_SELF_TEST_TIMEOUT", 0.05):
                with self.assertRaisesRegex(bootstrap.BootstrapError, "timed out"):
                    bootstrap._production_self_test(generation(Path(temporary), source))
        with tempfile.TemporaryDirectory() as temporary:
            source = (
                b"import os\n"
                b"def support_self_test():\n"
                b"    return ('PARENT_SECRET' not in os.environ and "
                b"os.path.realpath(os.getcwd()) == os.path.realpath(os.environ['HOME']) "
                b"and os.environ['HOME'] == os.environ['TMPDIR'])\n"
            )
            with mock.patch.dict(os.environ, {"PARENT_SECRET": "must-not-leak"}, clear=False):
                bootstrap._production_self_test(generation(Path(temporary), source))

    def test_review_medium1_rejections_do_not_publish_and_explicit_retry_commits_once(self) -> None:
        import importlib

        bootstrap = importlib.import_module("scripts.support_bootstrap")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            archive, manifest = write_artifacts(root)
            cli.install(archive, manifest, home=home,
                        platform=FakePlatform(), effective_uid=501)
            paths = cli._paths(home)
            fixture = root / "fixture"
            fixture.mkdir()
            module = fixture / "desktop_user_cli.py"
            verifier = fixture / "verify-app-archive.py"
            base_module = Path(cli.__file__).read_bytes()
            base_verifier = (Path(cli.__file__).parent / "verify-app-archive.py").read_bytes()

            def write_candidate(suffix: bytes) -> None:
                module.write_bytes(base_module + suffix)
                verifier.write_bytes(base_verifier + suffix)
                module.chmod(0o600)
                verifier.chmod(0o600)

            rejected = (
                ("lower", "0.1.4", b"\n# lower\n", [os.fspath(paths.launcher)]),
                ("same-substitution", "0.1.5", b"\n# same different\n", [os.fspath(paths.launcher)]),
                ("malformed", "0.1.6", b"\nthis is not python !!!\n", [os.fspath(paths.launcher)]),
                ("empty-argv", "0.1.6", b"\n# empty argv\n", []),
            )
            for label, version, suffix, argv in rejected:
                with self.subTest(label=label):
                    write_candidate(suffix)
                    before = identity_tree_snapshot(paths.support)
                    with self.assertRaises((cli.DesktopCLIError, SyntaxError)):
                        cli.stage_local_support_update(
                            module, verifier, support_version=version,
                            release_tag=f"v{version}", argv=argv, home=home,
                            exec_launcher=lambda *_: None,
                        )
                    self.assertEqual(identity_tree_snapshot(paths.support), before)

            write_candidate(b"\n# exact retry target\n")
            target = cli.stage_local_support_update(
                module, verifier, support_version="0.1.6", release_tag="v0.1.6",
                argv=[os.fspath(paths.launcher)], home=home,
                exec_launcher=lambda *_: None,
            )
            bootstrap.process_pending(
                home=home,
                invocation_argv=[os.fspath(paths.launcher)],
                self_test_runner=lambda _: (_ for _ in ()).throw(
                    bootstrap.BootstrapError("first attempt failed")
                ),
            )
            failed_before = identity_tree_snapshot(paths.support)
            with self.assertRaises(cli.DesktopCLIError):
                cli.stage_local_support_update(
                    module, verifier, support_version="0.1.6", release_tag="v0.1.6",
                    argv=[os.fspath(paths.launcher)], home=home,
                    exec_launcher=lambda *_: None,
                )
            self.assertEqual(identity_tree_snapshot(paths.support), failed_before)
            retried = cli.stage_local_support_update(
                module, verifier, support_version="0.1.6", release_tag="v0.1.6",
                argv=[os.fspath(paths.launcher)], home=home, explicit_retry=True,
                exec_launcher=lambda *_: None,
            )
            self.assertEqual(retried, target)
            selected = bootstrap.process_pending(
                home=home,
                invocation_argv=[os.fspath(paths.launcher)],
                self_test_runner=lambda _: None,
            )
            self.assertEqual(selected.generation_id, target)
            self.assertFalse(paths.support_pending.exists())
            self.assertNotIn(
                target,
                [item.generation_id for item in cli.parse_support_state(
                    paths.support_state.read_bytes()
                ).failed_generations],
            )

    def test_review_medium2_canonical_wrong_hash_state_fails_before_import_or_mutation(self) -> None:
        import importlib

        bootstrap = importlib.import_module("scripts.support_bootstrap")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            archive, manifest = write_artifacts(root)
            cli.install(archive, manifest, home=home,
                        platform=FakePlatform(), effective_uid=501)
            paths = cli._paths(home)
            state = cli.parse_support_state(paths.support_state.read_bytes())
            paths.support_state.write_bytes(cli.support_state_bytes(dataclasses.replace(
                state, high_water_manifest_sha256="0" * 64,
            )))
            paths.support_state.chmod(0o600)
            before = identity_tree_snapshot(paths.support)
            imported: list[bool] = []
            with self.assertRaises(bootstrap.BootstrapError):
                bootstrap.run_launcher(
                    ["version"], home=home,
                    installed_runner=lambda *_: imported.append(True) or 0,
                )
            self.assertEqual(imported, [])
            self.assertEqual(identity_tree_snapshot(paths.support), before)

    def test_review_medium2_state_relationship_matrix_and_authenticated_local_rollback(self) -> None:
        import importlib

        bootstrap = importlib.import_module("scripts.support_bootstrap")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            archive, manifest = write_artifacts(root)
            cli.install(
                archive, manifest, home=home, platform=FakePlatform(), effective_uid=501,
            )
            paths = cli._paths(home)
            base = cli.parse_support_state(paths.support_state.read_bytes())
            source_manifest = cli.parse_support_manifest(
                (paths.support_versions / base.last_good_generation /
                 "support-manifest.json").read_bytes()
            )
            fixture = root / "fixture"
            fixture.mkdir()
            module = fixture / "desktop_user_cli.py"
            verifier = fixture / "verify-app-archive.py"
            module.write_bytes(Path(cli.__file__).read_bytes() + b"\n# semantic target\n")
            verifier.write_bytes(
                (Path(cli.__file__).parent / "verify-app-archive.py").read_bytes()
                + b"\n# semantic target\n"
            )
            module.chmod(0o600)
            verifier.chmod(0o600)
            target_id = cli.stage_local_support_update(
                module, verifier, support_version="0.1.6", release_tag="v0.1.6",
                argv=[os.fspath(paths.launcher), "version"], home=home,
                exec_launcher=lambda *_: None,
            )
            paths.support_pending.unlink()
            target_manifest = cli.parse_support_manifest(
                (paths.support_versions / target_id / "support-manifest.json").read_bytes()
            )
            source_failed = cli.FailedSupportGeneration(
                base.last_good_generation, source_manifest.manifest_sha256,
            )
            target_failed = cli.FailedSupportGeneration(
                target_id, target_manifest.manifest_sha256,
            )
            impossible = {
                "last-good-also-failed": dataclasses.replace(
                    base, failed_generations=(source_failed,),
                ),
                "failed-wrong-hash": dataclasses.replace(
                    base, failed_generations=(dataclasses.replace(
                        target_failed, manifest_sha256="0" * 64,
                    ),),
                ),
                "higher-without-failed-proof": dataclasses.replace(
                    base, high_water_version="0.1.6",
                    high_water_manifest_sha256=target_manifest.manifest_sha256,
                ),
                "higher-with-wrong-failed-proof": dataclasses.replace(
                    base, high_water_version="0.1.6",
                    high_water_manifest_sha256=target_manifest.manifest_sha256,
                    failed_generations=(dataclasses.replace(
                        target_failed, manifest_sha256="0" * 64,
                    ),),
                ),
            }
            for label, state in impossible.items():
                with self.subTest(label=label):
                    paths.support_state.write_bytes(cli.support_state_bytes(state))
                    paths.support_state.chmod(0o600)
                    before = identity_tree_snapshot(paths.support)
                    imported: list[bool] = []
                    with self.assertRaises((cli.DesktopCLIError, bootstrap.BootstrapError)):
                        bootstrap.run_launcher(
                            ["version"], home=home,
                            installed_runner=lambda *_: imported.append(True) or 0,
                        )
                    self.assertEqual(imported, [])
                    self.assertEqual(identity_tree_snapshot(paths.support), before)
            authenticated = dataclasses.replace(
                base, high_water_version="0.1.6",
                high_water_manifest_sha256=target_manifest.manifest_sha256,
                failed_generations=(target_failed,),
            )
            paths.support_state.write_bytes(cli.support_state_bytes(authenticated))
            paths.support_state.chmod(0o600)
            self.assertEqual(
                bootstrap.process_pending(home=home).generation_id,
                base.last_good_generation,
            )
            self.assertEqual(
                cli._validate_owned_cli(paths).manifest.generation_id,
                base.last_good_generation,
            )

            duplicate_value = cli._support_state_value(authenticated)
            duplicate_value["failed_generations"] = [
                duplicate_value["failed_generations"][0],
                duplicate_value["failed_generations"][0],
            ]
            duplicate_bytes = (
                json.dumps(duplicate_value, sort_keys=True, separators=(",", ":")) + "\n"
            ).encode("ascii")
            with self.assertRaises(cli.DesktopCLIError):
                cli.parse_support_state(duplicate_bytes)
            with self.assertRaises(bootstrap.BootstrapError):
                bootstrap._parse_state(duplicate_bytes)

    def test_review_medium3_app_only_uninstall_ignores_every_support_state(self) -> None:
        for case in ("valid", "absent", "tampered", "symlink", "unknown"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                home = root / "home"
                home.mkdir()
                archive, manifest = write_artifacts(root)
                cli.install(archive, manifest, home=home,
                            platform=FakePlatform(), effective_uid=501)
                paths = cli._paths(home)
                outside: Path | None = None
                if case == "absent":
                    shutil.rmtree(paths.support)
                elif case == "tampered":
                    active = paths.support / os.readlink(paths.support_current)
                    (active / "desktop_user_cli.py").write_text("tampered", encoding="utf-8")
                    (active / "desktop_user_cli.py").chmod(0o600)
                elif case == "symlink":
                    outside = root / "outside-support"
                    paths.support.rename(outside)
                    paths.support.symlink_to(outside, target_is_directory=True)
                elif case == "unknown":
                    (paths.support / "KEEP").write_text("foreign", encoding="utf-8")
                support_before = identity_tree_snapshot(paths.support)
                outside_before = identity_tree_snapshot(outside) if outside is not None else ()
                launcher_before = identity_tree_snapshot(paths.launcher)
                cli.uninstall_version("0.1.5", home=home, effective_uid=501)
                self.assertFalse((paths.versions / "0.1.5").exists())
                self.assertEqual(identity_tree_snapshot(paths.support), support_before)
                self.assertEqual(identity_tree_snapshot(paths.launcher), launcher_before)
                if outside is not None:
                    self.assertEqual(identity_tree_snapshot(outside), outside_before)

    def test_repair_self_test_violation_record_cannot_be_cleared_by_candidate(self) -> None:
        import importlib

        bootstrap = importlib.import_module("scripts.support_bootstrap")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = (
                b"import __main__\n"
                b"import os\n"
                b"def support_self_test():\n"
                b"    for cell in (getattr(os._exit, '__closure__', None) or ()):\n"
                b"        hidden_exit = cell.cell_contents\n"
                b"        if getattr(hidden_exit, '__name__', '') == '_exit':\n"
                b"            hidden_exit(0)\n"
                b"    try:\n"
                b"        os._exit(0)\n"
                b"    except PermissionError:\n"
                b"        pass\n"
                b"    __main__.violations.clear()\n"
                b"    return True\n"
            )
            generation = bootstrap.Generation(
                root / "1.0.0-000000000000", "1.0.0", "1.0.0-000000000000",
                "0" * 64, (1, 1), (), b"manifest", (
                    ("desktop_user_cli.py", source),
                    ("verify-app-archive.py", b"# verifier\n"),
                ),
            )
            with self.assertRaisesRegex(bootstrap.BootstrapError, "isolated exit"):
                bootstrap._production_self_test(generation)

    def test_review_fix_self_test_terminal_marker_is_wrapper_owned(self) -> None:
        import importlib

        bootstrap = importlib.import_module("scripts.support_bootstrap")

        def generation(root: Path, source: bytes) -> object:
            return bootstrap.Generation(
                root / "1.0.0-000000000000", "1.0.0", "1.0.0-000000000000",
                "0" * 64, (1, 1), (), b"manifest", (
                    ("desktop_user_cli.py", source),
                    ("verify-app-archive.py", b"# verifier\n"),
                ),
            )

        inherited_fd = (
            b"def inherited_fd():\n"
            b"    for fd in range(3, 64):\n"
            b"        try:\n"
            b"            os.fstat(fd)\n"
            b"        except OSError:\n"
            b"            continue\n"
            b"        return fd\n"
            b"    raise RuntimeError('inherited audit fd not found')\n"
            b"audit_fd = inherited_fd()\n"
        )
        replace_main_finalizers = (
            b"for name in ('finish_policy', 'install_policy', 'run_candidate', 'violations'):\n"
            b"    setattr(__main__, name, lambda *_: None)\n"
            b"def support_self_test():\n"
            b"    return True\n"
        )
        rejected = {
            "exact-review-forgery": (
                b"import os, sys, __main__\n"
                b"os.write(int(sys.argv[2]), b'T')\n"
                b"__main__.finish_policy = lambda *_: None\n"
                b"def support_self_test():\n"
                b"    return True\n"
            ),
            "inherited-os-write": (
                b"import os, __main__\n" + inherited_fd
                + b"try:\n    os.write(audit_fd, b'T')\nexcept PermissionError:\n    pass\n"
                + replace_main_finalizers
            ),
            "inherited-posix-write": (
                b"import os, posix, __main__\n" + inherited_fd
                + b"try:\n    posix.write(audit_fd, b'T')\nexcept PermissionError:\n    pass\n"
                + replace_main_finalizers
            ),
            "partial-marker": (
                b"import os, __main__\n" + inherited_fd
                + b"try:\n    os.write(audit_fd, b'')\nexcept PermissionError:\n    pass\n"
                + replace_main_finalizers
            ),
            "extra-marker": (
                b"import os, __main__\n" + inherited_fd
                + b"try:\n    os.write(audit_fd, b'TT')\nexcept PermissionError:\n    pass\n"
                + replace_main_finalizers
            ),
            "forged-marker-then-close": (
                b"import os, __main__\n" + inherited_fd
                + b"try:\n    os.write(audit_fd, b'T')\nexcept PermissionError:\n    pass\n"
                + b"os.close(audit_fd)\n" + replace_main_finalizers
            ),
            "candidate-frame-introspection": (
                b"import sys\n"
                b"try:\n    sys._getframe()\nexcept PermissionError:\n    pass\n"
                b"def support_self_test():\n    return True\n"
            ),
            "wrapper-frame-introspection": (
                b"import sys\n"
                b"def support_self_test():\n"
                b"    sys._getframe(1)\n"
                b"    return True\n"
            ),
        }
        for label, source in rejected.items():
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                with self.assertRaisesRegex(bootstrap.BootstrapError, "isolated exit"):
                    bootstrap._production_self_test(generation(Path(temporary), source))

        accepted = {
            "clean-true": b"def support_self_test():\n    return True\n",
            "clean-true-after-main-global-replacement": (
                b"import __main__\n"
                b"for name in ('finish_policy', 'install_policy', 'run_candidate', 'violations'):\n"
                b"    setattr(__main__, name, lambda *_: None)\n"
                b"def support_self_test():\n    return True\n"
            ),
        }
        for label, source in accepted.items():
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                bootstrap._production_self_test(generation(Path(temporary), source))

    def test_review_fix_forged_self_test_cannot_commit_pending_transaction(self) -> None:
        import importlib

        bootstrap = importlib.import_module("scripts.support_bootstrap")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            archive, manifest = write_artifacts(root)
            cli.install(
                archive, manifest, home=home, platform=FakePlatform(), effective_uid=501,
            )
            paths = cli._paths(home)
            source = os.readlink(paths.support_current).removeprefix("versions/")
            current_identity = cli._identity(paths.support_current)
            state_before = cli.parse_support_state(paths.support_state.read_bytes())
            app_before = tuple(identity_tree_snapshot(path) for path in (
                paths.versions, paths.receipts, paths.current, paths.update_cache,
            ))
            fixture = root / "fixture"
            fixture.mkdir()
            module = fixture / "desktop_user_cli.py"
            verifier = fixture / "verify-app-archive.py"
            module.write_bytes(
                Path(cli.__file__).read_bytes()
                + b"\nimport os, sys, __main__\n"
                + b"os.write(int(sys.argv[2]), b'T')\n"
                + b"__main__.finish_policy = lambda *_: None\n"
                + b"def support_self_test():\n    return True\n"
            )
            verifier.write_bytes(
                (Path(cli.__file__).parent / "verify-app-archive.py").read_bytes()
                + b"\n# forged self-test transaction\n"
            )
            module.chmod(0o600)
            verifier.chmod(0o600)
            target = cli.stage_local_support_update(
                module, verifier, support_version="0.1.6", release_tag="v0.1.6",
                argv=[os.fspath(paths.launcher), "version"], home=home,
                exec_launcher=lambda *_: None,
            )
            self.assertTrue(paths.support_pending.exists())
            selected = bootstrap.process_pending(
                home=home,
                invocation_argv=[os.fspath(paths.launcher), "version"],
            )
            self.assertEqual(selected.generation_id, source)
            self.assertEqual(os.readlink(paths.support_current), f"versions/{source}")
            self.assertEqual(cli._identity(paths.support_current), current_identity)
            state_after = cli.parse_support_state(paths.support_state.read_bytes())
            self.assertEqual(state_after.last_good_generation, state_before.last_good_generation)
            self.assertEqual(state_after.high_water_version, state_before.high_water_version)
            self.assertEqual(
                state_after.high_water_manifest_sha256,
                state_before.high_water_manifest_sha256,
            )
            self.assertIn(target, [item.generation_id for item in state_after.failed_generations])
            self.assertFalse(paths.support_pending.exists())
            self.assertEqual(
                tuple(identity_tree_snapshot(path) for path in (
                    paths.versions, paths.receipts, paths.current, paths.update_cache,
                )),
                app_before,
            )

    def test_repair_pending_transaction_is_bound_to_exact_invocation_argv(self) -> None:
        import importlib

        bootstrap = importlib.import_module("scripts.support_bootstrap")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            archive, manifest = write_artifacts(root)
            cli.install(
                archive, manifest, home=home, platform=FakePlatform(), effective_uid=501,
            )
            paths = cli._paths(home)
            source = os.readlink(paths.support_current).removeprefix("versions/")
            fixture = root / "fixture"
            fixture.mkdir()
            module = fixture / "desktop_user_cli.py"
            verifier = fixture / "verify-app-archive.py"
            module.write_bytes(Path(cli.__file__).read_bytes() + b"\n# argv-bound target\n")
            verifier.write_bytes(
                (Path(cli.__file__).parent / "verify-app-archive.py").read_bytes()
                + b"\n# argv-bound target\n"
            )
            module.chmod(0o600)
            verifier.chmod(0o600)
            target = cli.stage_local_support_update(
                module, verifier, support_version="0.1.6", release_tag="v0.1.6",
                argv=[os.fspath(paths.launcher), "version"], home=home,
                exec_launcher=lambda *_: None,
            )
            delegated: list[tuple[Path, list[str]]] = []
            with self.assertRaisesRegex(bootstrap.BootstrapError, "invocation"):
                bootstrap.run_launcher(
                    ["doctor"], home=home, self_test_runner=lambda _: None,
                    installed_runner=lambda path, arguments: (
                        delegated.append((path, list(arguments))) or 0
                    ),
                )
            self.assertEqual(delegated, [])
            self.assertTrue(paths.support_pending.exists())
            self.assertEqual(
                os.readlink(paths.support_current), f"versions/{source}",
            )
            self.assertEqual(
                bootstrap.run_launcher(
                    ["version"], home=home, self_test_runner=lambda _: None,
                    installed_runner=lambda path, arguments: (
                        delegated.append((path, list(arguments))) or 23
                    ),
                ), 23,
            )
            self.assertEqual(delegated, [(
                paths.support_versions / target / "desktop_user_cli.py", ["version"],
            )])
            self.assertFalse(paths.support_pending.exists())

    def test_review_fix_staging_canonicalizes_every_replayable_full_argv(self) -> None:
        import importlib

        bootstrap = importlib.import_module("scripts.support_bootstrap")
        cases = (
            ("bare-argv0", "bare", ["version"], False),
            ("alternate-absolute-argv0", "absolute", ["version"], False),
            ("default-argv", "default", ["version"], False),
            ("explicit-retry", "bare", ["version"], True),
            ("argv0-that-looks-like-an-argument", "argument-looking", [], False),
        )
        for label, representation, arguments, explicit_retry in cases:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                home = root / "home"
                home.mkdir()
                archive, manifest = write_artifacts(root)
                cli.install(
                    archive, manifest, home=home, platform=FakePlatform(),
                    effective_uid=501,
                )
                paths = cli._paths(home)
                source = os.readlink(paths.support_current).removeprefix("versions/")
                source_identity = cli._identity(paths.support_current)
                fixture = root / "fixture"
                fixture.mkdir()
                module = fixture / "desktop_user_cli.py"
                verifier = fixture / "verify-app-archive.py"
                suffix = f"\n# canonical argv target: {label}\n".encode("ascii")
                module.write_bytes(Path(cli.__file__).read_bytes() + suffix)
                verifier.write_bytes(
                    (Path(cli.__file__).parent / "verify-app-archive.py").read_bytes()
                    + suffix
                )
                module.chmod(0o600)
                verifier.chmod(0o600)
                if representation == "bare":
                    supplied_argv = ["lingtai-desktop", *arguments]
                elif representation == "absolute":
                    supplied_argv = [os.fspath(root / "alternate-lingtai-desktop"), *arguments]
                elif representation == "argument-looking":
                    supplied_argv = ["version"]
                else:
                    supplied_argv = ["default-lingtai-desktop", *arguments]
                expected_argv = [os.fspath(paths.launcher), *arguments]
                executed: list[tuple[Path, list[str], dict[str, str]]] = []

                def capture_exec(
                        launcher: Path, argv: list[str], environment: dict[str, str],
                ) -> None:
                    executed.append((launcher, list(argv), dict(environment)))

                stage_kwargs = dict(
                    support_version="0.1.6", release_tag="v0.1.6", home=home,
                    explicit_retry=explicit_retry, exec_launcher=capture_exec,
                )
                if representation == "default":
                    with mock.patch.object(cli.sys, "argv", supplied_argv):
                        target = cli.stage_local_support_update(
                            module, verifier, argv=None, **stage_kwargs,
                        )
                else:
                    target = cli.stage_local_support_update(
                        module, verifier, argv=supplied_argv, **stage_kwargs,
                    )
                pending = cli.parse_support_pending(paths.support_pending.read_bytes())
                self.assertEqual(pending.explicit_retry, explicit_retry)

                with self.assertRaisesRegex(bootstrap.BootstrapError, "invocation"):
                    bootstrap.run_launcher(
                        ["doctor"], home=home,
                        installed_runner=lambda *_: self.fail(
                            "doctor substitution reached the installed runner"
                        ),
                    )
                self.assertTrue(paths.support_pending.exists())
                self.assertEqual(cli._identity(paths.support_current), source_identity)

                delegated: list[tuple[Path, list[str]]] = []
                self.assertEqual(
                    bootstrap.run_launcher(
                        arguments, home=home,
                        installed_runner=lambda path, invocation: (
                            delegated.append((path, list(invocation))) or 43
                        ),
                    ),
                    43,
                )
                self.assertEqual(executed, [(
                    paths.launcher,
                    expected_argv,
                    mock.ANY,
                )])
                self.assertEqual(
                    executed[0][2][cli.SUPPORT_REEXEC_MARKER],
                    "1",
                )
                self.assertEqual(
                    pending.requested_argv_sha256,
                    cli._argv_sha256(expected_argv),
                )
                self.assertEqual(delegated, [(
                    paths.support_versions / target / "desktop_user_cli.py",
                    arguments,
                )])
                self.assertFalse(paths.support_pending.exists())
                self.assertEqual(
                    os.readlink(paths.support_current), f"versions/{target}",
                )
                state = cli.parse_support_state(paths.support_state.read_bytes())
                self.assertEqual(state.last_good_generation, target)
                self.assertNotEqual(state.last_good_generation, source)

    def test_repair_app_post_current_failure_restores_exact_previous_pointer(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            platform = FakePlatform()
            archive1, manifest1 = write_artifacts(root, "0.1.5")
            cli.install(
                archive1, manifest1, home=home, platform=platform, effective_uid=501,
            )
            paths = cli._paths(home)
            previous_identity = cli._identity(paths.current)
            previous_target = os.readlink(paths.current)
            newer = root / "newer"
            newer.mkdir()
            archive2, manifest2 = write_artifacts(newer, "0.1.6")
            with mock.patch.object(
                    cli, "_FAILPOINT", "app-current-post-visible",
            ), self.assertRaisesRegex(cli.DesktopCLIError, "post-publication"):
                cli.install(
                    archive2, manifest2, home=home, platform=platform,
                    effective_uid=501, update=True,
                )
            self.assertEqual(cli._identity(paths.current), previous_identity)
            self.assertEqual(os.readlink(paths.current), previous_target)
            self.assertFalse((paths.versions / "0.1.6").exists())
            self.assertFalse((paths.receipts / "0.1.6.json").exists())
            self.assertEqual(cli._active(paths)[0], "0.1.5")

    def test_repair_mutated_self_tested_target_recovers_last_good_without_target_trust(self) -> None:
        import importlib

        bootstrap = importlib.import_module("scripts.support_bootstrap")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            archive, manifest = write_artifacts(root)
            cli.install(
                archive, manifest, home=home, platform=FakePlatform(), effective_uid=501,
            )
            paths = cli._paths(home)
            source = os.readlink(paths.support_current).removeprefix("versions/")
            fixture = root / "fixture"
            fixture.mkdir()
            module = fixture / "desktop_user_cli.py"
            verifier = fixture / "verify-app-archive.py"
            module.write_bytes(Path(cli.__file__).read_bytes() + b"\n# mutable target\n")
            verifier.write_bytes(
                (Path(cli.__file__).parent / "verify-app-archive.py").read_bytes()
                + b"\n# mutable target\n"
            )
            module.chmod(0o600)
            verifier.chmod(0o600)
            target = cli.stage_local_support_update(
                module, verifier, support_version="0.1.6", release_tag="v0.1.6",
                argv=[os.fspath(paths.launcher), "version"], home=home,
                exec_launcher=lambda *_: None,
            )

            def mutate_after_self_test(generation: object) -> None:
                self.assertEqual(generation.generation_id, target)
                (generation.path / "desktop_user_cli.py").chmod(0o644)

            selected = bootstrap.process_pending(
                home=home,
                invocation_argv=[os.fspath(paths.launcher), "version"],
                self_test_runner=mutate_after_self_test,
            )
            self.assertEqual(selected.generation_id, source)
            self.assertEqual(
                os.readlink(paths.support_current), f"versions/{source}",
            )
            self.assertFalse(paths.support_pending.exists())
            delegated: list[Path] = []
            self.assertEqual(bootstrap.run_launcher(
                ["version"], home=home,
                installed_runner=lambda module_path, _arguments: (
                    delegated.append(module_path) or 31
                ),
            ), 31)
            self.assertEqual(delegated, [
                paths.support_versions / source / "desktop_user_cli.py",
            ])

    def test_repair_app_receipt_rejects_hardlink_and_wrong_mode_on_all_authority_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            archive, manifest = write_artifacts(root)
            cli.install(
                archive, manifest, home=home, platform=FakePlatform(), effective_uid=501,
            )
            paths = cli._paths(home)
            receipt = paths.receipts / "0.1.5.json"
            alias = root / "receipt-hardlink"
            os.link(receipt, alias)
            receipt.chmod(0o644)
            self.assertEqual(receipt.stat().st_nlink, 2)
            with self.assertRaisesRegex(cli.DesktopCLIError, "ownership, mode, or size"):
                cli._active(paths)
            with self.assertRaisesRegex(cli.DesktopCLIError, "ownership, mode, or size"):
                cli.doctor(home=home)
            alias.unlink()
            receipt.chmod(0o600)
            value = json.loads(receipt.read_bytes())
            receipt.write_bytes(json.dumps(value, sort_keys=True).encode())
            receipt.chmod(0o600)
            with self.assertRaisesRegex(cli.DesktopCLIError, "canonical"):
                cli._active(paths)
            with self.assertRaisesRegex(cli.DesktopCLIError, "canonical"):
                cli.doctor(home=home)

    def test_repair_support_release_tag_must_exactly_match_support_version(self) -> None:
        payloads = {
            "desktop_user_cli.py": b"def installed_main(argv=None):\n    return 0\n",
            "verify-app-archive.py": b"# verifier\n",
        }
        with self.assertRaisesRegex(cli.DesktopCLIError, "tag.*version|version.*tag"):
            cli.build_support_manifest_bytes("0.1.6", "v9.9.9", payloads)

        valid = cli.parse_support_manifest(
            cli.build_support_manifest_bytes("0.1.6", "v0.1.6", payloads)
        )
        identity = cli._support_manifest_identity_value(
            "0.1.6", "v9.9.9", cli.SUPPORT_REPOSITORY,
            cli.SUPPORT_BOOTSTRAP_PROTOCOL, cli.SUPPORT_BOOTSTRAP_PROTOCOL,
            valid.files,
        )
        mismatched = dict(identity)
        mismatched["generation_id"] = cli._support_generation_id(identity)
        raw = cli._canonical_json_bytes(
            mismatched, cli.MAX_SUPPORT_MANIFEST_BYTES, "support manifest",
        )
        with self.assertRaisesRegex(cli.DesktopCLIError, "tag.*version|version.*tag"):
            cli.parse_support_manifest(raw)

        import importlib
        bootstrap = importlib.import_module("scripts.support_bootstrap")
        with tempfile.TemporaryDirectory() as temporary:
            manifest = Path(temporary) / "support-manifest.json"
            manifest.write_bytes(raw)
            manifest.chmod(0o600)
            with self.assertRaisesRegex(bootstrap.BootstrapError, "release identity"):
                bootstrap._manifest(manifest)

    def test_repair_app_current_replacement_race_preserves_racer_pointer(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            platform = FakePlatform()
            archive1, manifest1 = write_artifacts(root, "0.1.5")
            cli.install(
                archive1, manifest1, home=home, platform=platform, effective_uid=501,
            )
            paths = cli._paths(home)
            previous_target = os.readlink(paths.current)
            newer = root / "newer"
            newer.mkdir()
            archive2, manifest2 = write_artifacts(newer, "0.1.6")
            original_symlink = os.symlink
            raced_identity: list[tuple[int, int]] = []

            def race_before_publication(
                    target: str, link_name: os.PathLike[str] | str,
                    *args: object, **kwargs: object) -> None:
                link = Path(link_name)
                if link.parent == paths.root and link.name.startswith(".current-"):
                    paths.current.unlink()
                    original_symlink(previous_target, paths.current)
                    raced_identity.append(cli._identity(paths.current))
                original_symlink(target, link_name, *args, **kwargs)

            with mock.patch.object(cli.os, "symlink", side_effect=race_before_publication), \
                 self.assertRaisesRegex(cli.DesktopCLIError, "raced|changed|replaced"):
                cli.install(
                    archive2, manifest2, home=home, platform=platform,
                    effective_uid=501, update=True,
                )
            self.assertEqual(len(raced_identity), 1)
            self.assertEqual(cli._identity(paths.current), raced_identity[0])
            self.assertEqual(os.readlink(paths.current), previous_target)
            self.assertFalse((paths.versions / "0.1.6").exists())
            self.assertFalse((paths.receipts / "0.1.6.json").exists())
            self.assertEqual(cli._active(paths)[0], "0.1.5")

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
