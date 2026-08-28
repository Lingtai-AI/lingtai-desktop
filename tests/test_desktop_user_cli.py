#!/usr/bin/env python3
"""Hermetic contracts for the user-level LingTai Desktop installer/launcher."""

from __future__ import annotations

import hashlib
import io
import json
import os
import plistlib
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
                managed, managed / "cli", managed / "versions", managed / "receipts"
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

    def test_identical_verifier_racer_survives_failed_publication_rollback(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            archive, manifest = write_artifacts(root)
            verifier = home / ".local/share/lingtai-desktop/cli/verify-app-archive.py"
            real_link = os.link
            racer_bytes: bytes | None = None

            def race_verifier(source: Path, destination: Path, **kwargs: object) -> None:
                nonlocal racer_bytes
                if Path(destination) == verifier:
                    racer_bytes = Path(source).read_bytes()
                    verifier.write_bytes(racer_bytes)
                    verifier.chmod(0o600)
                real_link(source, destination, **kwargs)

            with mock.patch.object(cli.os, "link", side_effect=race_verifier):
                with self.assertRaisesRegex(cli.DesktopCLIError, "refusing to overwrite"):
                    cli.install(archive, manifest, home=home,
                                platform=FakePlatform(), effective_uid=501)
            self.assertIsNotNone(racer_bytes)
            self.assertEqual(verifier.read_bytes(), racer_bytes)

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
            self.assertTrue((managed / "cli/desktop_user_cli.py").is_file())
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
            verifier = home / ".local/share/lingtai-desktop/cli/verify-app-archive.py"
            verifier.write_text("tampered verifier")
            with self.assertRaisesRegex(cli.DesktopCLIError, "unrelated launcher"):
                cli.run_installed(["doctor"], home=home, platform=platform,
                                  transport=transport, tty=lambda: False,
                                  output=out.append)

    def test_update_failure_matrix_preserves_old_current_and_owned_bytes(self) -> None:
        for failure in ("verifier", "extract", "receipt", "launcher", "current"):
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
