#!/usr/bin/env python3
"""Phase 3 contracts for the independently managed Desktop support plane."""

from __future__ import annotations

import hashlib
import json
import os
import tempfile
import unittest
from pathlib import Path

from scripts import desktop_user_cli as cli
from tests.test_desktop_user_cli import (
    FakePlatform, FakeReleaseTransport, tree_snapshot, write_artifacts,
)


def support_release_transport(
        payloads: dict[str, bytes], version: str = "0.1.6", *,
        manifest_bytes: bytes | None = None,
        metadata_mutator=None) -> tuple[FakeReleaseTransport, bytes]:
    manifest_bytes = manifest_bytes or cli.build_support_manifest_bytes(
        version, f"v{version}", payloads,
    )
    assets: list[dict[str, object]] = []
    routes: dict[str, tuple[int, dict[str, str], bytes]] = {}
    all_payloads = {cli.SUPPORT_MANIFEST_NAME: manifest_bytes, **payloads}
    for index, (name, content) in enumerate(all_payloads.items()):
        url = (
            "https://github.com/Lingtai-AI/lingtai-desktop/releases/"
            f"download/v{version}/{name}"
        )
        final = (
            "https://release-assets.githubusercontent.com/"
            f"github-production-release-asset/support-{index}?token=offline"
        )
        assets.append({"name": name, "browser_download_url": url, "size": len(content)})
        routes[url] = (302, {"Location": final}, b"")
        routes[final] = (200, {"Content-Length": str(len(content))}, content)
    metadata: dict[str, object] = {
        "tag_name": f"v{version}", "draft": False, "prerelease": False,
        "assets": assets,
    }
    if metadata_mutator is not None:
        metadata_mutator(metadata)
    raw = json.dumps(metadata, separators=(",", ":")).encode()
    routes[cli.OFFICIAL_LATEST_RELEASE_URL] = (
        200, {"Content-Length": str(len(raw))}, raw,
    )
    routes[cli.OFFICIAL_RELEASE_TAG_URL.format(version)] = (
        200, {"Content-Length": str(len(raw))}, raw,
    )
    return FakeReleaseTransport(routes), manifest_bytes


class DesktopSupportUpdatePhase3Test(unittest.TestCase):
    def setUp(self) -> None:
        repository = Path(__file__).resolve().parents[1]
        self.payloads = {
            cli.SUPPORT_PAYLOAD_NAMES[0]: (repository / "scripts/desktop_user_cli.py").read_bytes(),
            cli.SUPPORT_PAYLOAD_NAMES[1]: (repository / "scripts/verify-app-archive.py").read_bytes(),
        }

    def test_official_support_discovery_and_download_bind_exact_release(self) -> None:
        transport, manifest_bytes = support_release_transport(self.payloads)
        release = cli.discover_official_support_release(transport=transport)
        self.assertEqual(release.version, "0.1.6")
        self.assertEqual(release.manifest.release_tag, "v0.1.6")
        self.assertEqual(release.manifest.repository, cli.OFFICIAL_REPOSITORY)
        self.assertEqual(release.manifest_bytes, manifest_bytes)
        self.assertEqual(set(release.payload_assets), set(cli.SUPPORT_PAYLOAD_NAMES))
        self.assertEqual([call[0] for call in transport.calls[:2]], [
            cli.OFFICIAL_LATEST_RELEASE_URL,
            "https://github.com/Lingtai-AI/lingtai-desktop/releases/"
            "download/v0.1.6/support-manifest.json",
        ])

        with cli.downloaded_official_support_release(transport=transport) as downloaded:
            downloaded_release, manifest_path, payload_paths = downloaded
            scratch = manifest_path.parent
            self.assertEqual(downloaded_release.manifest, release.manifest)
            self.assertEqual(manifest_path.read_bytes(), manifest_bytes)
            self.assertEqual(
                {name: path.read_bytes() for name, path in payload_paths.items()},
                self.payloads,
            )
            for path in [manifest_path, *payload_paths.values()]:
                facts = path.stat()
                self.assertEqual(facts.st_mode & 0o777, 0o600)
                self.assertEqual(facts.st_nlink, 1)
        self.assertFalse(scratch.exists())

    def test_support_discovery_rejects_duplicate_case_and_manifest_binding(self) -> None:
        def duplicate(metadata: dict[str, object]) -> None:
            assets = metadata["assets"]
            assert isinstance(assets, list)
            assets.append(dict(assets[0]))
        transport, _ = support_release_transport(self.payloads, metadata_mutator=duplicate)
        with self.assertRaisesRegex(cli.DesktopCLIError, "one exact support"):
            cli.discover_official_support_release(transport=transport)

        def wrong_case(metadata: dict[str, object]) -> None:
            assets = metadata["assets"]
            assert isinstance(assets, list)
            first = assets[0]
            assert isinstance(first, dict)
            first["name"] = "Support-Manifest.json"
        transport, _ = support_release_transport(self.payloads, metadata_mutator=wrong_case)
        with self.assertRaisesRegex(cli.DesktopCLIError, "ambiguous"):
            cli.discover_official_support_release(transport=transport)

        bad = json.loads(cli.build_support_manifest_bytes("0.1.6", "v0.1.6", self.payloads))
        bad["repository"] = "attacker/example"
        raw = (json.dumps(bad, sort_keys=True, separators=(",", ":")) + "\n").encode()
        transport, _ = support_release_transport(self.payloads, manifest_bytes=raw)
        with self.assertRaisesRegex(cli.DesktopCLIError, "repository"):
            cli.discover_official_support_release(transport=transport)

    def test_support_update_cache_is_independent_exact_and_records_decline(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest = write_artifacts(root, "0.1.5")
            home = root / "home"
            home.mkdir()
            cli.install(archive, manifest, home=home, platform=FakePlatform(), effective_uid=501)
            paths = cli._paths(home)
            app_cache_before = tree_snapshot(paths.update_cache)
            generation = cli.parse_support_manifest(
                cli.build_support_manifest_bytes("0.1.6", "v0.1.6", self.payloads)
            )
            check = cli.SupportUpdateCheck(
                checked_at=123, latest_support_version="0.1.6",
                release_tag="v0.1.6", generation_id=generation.generation_id,
                manifest_sha256=generation.manifest_sha256, declined=True,
            )
            cli._write_support_update_cache(paths, check)
            self.assertEqual(cli._read_support_update_cache(paths), check)
            self.assertEqual(tree_snapshot(paths.update_cache), app_cache_before)
            value = json.loads(paths.support_update_cache.read_bytes())
            self.assertEqual(set(value), cli.SUPPORT_UPDATE_CACHE_KEYS)
            self.assertTrue(value["declined"])
            paths.support_update_cache.chmod(0o644)
            with self.assertRaisesRegex(cli.DesktopCLIError, "ownership or mode"):
                cli._read_support_update_cache(paths)

    def test_verified_official_support_stage_publishes_pending_then_canonical_exec(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest = write_artifacts(root, "0.1.5")
            home = root / "home"
            home.mkdir()
            cli.install(archive, manifest, home=home, platform=FakePlatform(), effective_uid=501)
            paths = cli._paths(home)
            app_before = tree_snapshot(paths.versions)
            transport, _ = support_release_transport(self.payloads)
            executions: list[tuple[Path, list[str], dict[str, str]]] = []
            generation = cli.stage_official_support_update(
                home=home, transport=transport, effective_uid=501,
                argv=["relative-launcher", "version"], environment={"KEEP": "1"},
                exec_launcher=lambda path, argv, env: executions.append((path, argv, env)),
            )
            self.assertIsNotNone(generation)
            pending, _ = cli._read_support_pending(paths)
            self.assertEqual(pending.to_generation, generation)
            self.assertEqual(executions[0][0], paths.launcher)
            self.assertEqual(executions[0][1], [str(paths.launcher), "version"])
            self.assertEqual(executions[0][2][cli.SUPPORT_REEXEC_MARKER], "1")
            self.assertEqual(tree_snapshot(paths.versions), app_before)

    def test_local_app_pair_has_zero_support_transport_or_cache_effect(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            old_archive, old_manifest = write_artifacts(root, "0.1.5")
            release = root / "release"
            release.mkdir()
            new_archive, new_manifest = write_artifacts(release, "0.1.6")
            home = root / "home"
            home.mkdir()
            platform = FakePlatform()
            cli.install(old_archive, old_manifest, home=home, platform=platform, effective_uid=501)
            paths = cli._paths(home)
            support_before = tree_snapshot(paths.support)
            transport = FakeReleaseTransport({})
            self.assertEqual(cli.run_installed(
                ["update", "--archive", str(new_archive), "--manifest", str(new_manifest)],
                home=home, platform=platform, transport=transport, effective_uid=501,
            ), 0)
            self.assertEqual(transport.calls, [])
            self.assertEqual(tree_snapshot(paths.support), support_before)
            self.assertFalse(paths.support_update_cache.exists())

    def test_bootstrap_consumes_marker_before_import_and_exposes_one_shot_guard(self) -> None:
        source = (Path(__file__).resolve().parents[1] / "scripts/support_bootstrap.py").read_text()
        self.assertIn("_SUPPORT_REEXEC_CONSUMED", source)
        self.assertIn("os.environ.pop(SUPPORT_REEXEC_MARKER", source)
        self.assertNotIn("os.environ.get(SUPPORT_REEXEC_MARKER", source)


if __name__ == "__main__":
    unittest.main()
