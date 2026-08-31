#!/usr/bin/env python3
"""Phase 3 contracts for the independently managed Desktop support plane."""

from __future__ import annotations

import dataclasses
import hashlib
import json
import os
import tempfile
import unittest
from unittest import mock
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
        exact_transport, _ = support_release_transport(self.payloads)
        exact = cli.discover_official_support_release(
            "0.1.6", transport=exact_transport,
        )
        self.assertEqual(exact.version, "0.1.6")
        self.assertEqual(exact_transport.calls[0][0],
                         cli.OFFICIAL_RELEASE_TAG_URL.format("0.1.6"))

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

    def test_support_transport_hostility_matrix_is_fail_closed(self) -> None:
        def metadata(transport: FakeReleaseTransport) -> dict[str, object]:
            return json.loads(transport.routes[cli.OFFICIAL_LATEST_RELEASE_URL][2])
        def replace_metadata(transport: FakeReleaseTransport, value: dict[str, object]) -> None:
            raw = json.dumps(value, separators=(",", ":")).encode()
            transport.routes[cli.OFFICIAL_LATEST_RELEASE_URL] = (
                200, {"Content-Length": str(len(raw))}, raw,
            )

        for label, mutation, message in (
            ("draft", lambda value: value.__setitem__("draft", True), "stable release"),
            ("wrong-url-tail", lambda value: value["assets"][1].__setitem__(  # type: ignore[index,union-attr]
                "browser_download_url",
                "https://github.com/Lingtai-AI/lingtai-desktop/releases/download/"
                "v0.1.6/not-desktop_user_cli.py"), "tag and name"),
            ("unicode-url", lambda value: value["assets"][1].__setitem__(  # type: ignore[index,union-attr]
                "browser_download_url", "https://github.com/\N{SNOWMAN}"), "malformed"),
            ("untrusted-url", lambda value: value["assets"][1].__setitem__(  # type: ignore[index,union-attr]
                "browser_download_url", "https://example.com/payload"), "allowed official"),
            ("path-ambiguity", lambda value: value["assets"][1].__setitem__(  # type: ignore[index,union-attr]
                "name", "nested/desktop_user_cli.py"), "ambiguous"),
        ):
            with self.subTest(case=label):
                candidate, _ = support_release_transport(self.payloads)
                value = metadata(candidate)
                mutation(value)
                replace_metadata(candidate, value)
                with self.assertRaisesRegex(cli.DesktopCLIError, message):
                    cli.discover_official_support_release(transport=candidate)

        recursive = b"[" * 1200 + b"]" * 1200
        transport = FakeReleaseTransport({
            cli.OFFICIAL_LATEST_RELEASE_URL: (
                200, {"Content-Length": str(len(recursive))}, recursive,
            ),
        })
        with self.assertRaisesRegex(cli.DesktopCLIError, "bounded valid JSON|must be an object"):
            cli.discover_official_support_release(transport=transport)

        redirect, _ = support_release_transport(self.payloads)
        manifest_url = metadata(redirect)["assets"][0]["browser_download_url"]  # type: ignore[index,union-attr]
        assert isinstance(manifest_url, str)
        redirect.routes[manifest_url] = (302, {"Location": manifest_url}, b"")
        with self.assertRaisesRegex(cli.DesktopCLIError, "too many"):
            cli.discover_official_support_release(transport=redirect)

        mismatch, _ = support_release_transport(self.payloads)
        value = metadata(mismatch)
        value["assets"][0]["size"] += 1  # type: ignore[index,operator,union-attr]
        replace_metadata(mismatch, value)
        with self.assertRaisesRegex(cli.DesktopCLIError, "length does not match"):
            cli.discover_official_support_release(transport=mismatch)

        truncated, _ = support_release_transport(self.payloads)
        payload_url = next(
            url for url, route in truncated.routes.items()
            if "release-assets.githubusercontent.com" in url
            and route[2] == self.payloads[cli.SUPPORT_PAYLOAD_NAMES[1]]
        )
        status, _, body = truncated.routes[payload_url]
        truncated.routes[payload_url] = (
            status, {"Content-Length": str(len(body) - 1)}, body[:-1],
        )
        with self.assertRaisesRegex(cli.DesktopCLIError, "truncated|declared size|length does not match"):
            with cli.downloaded_official_support_release(transport=truncated):
                self.fail("truncated release yielded")

    def test_no_change_cache_and_failed_target_never_auto_retry(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest = write_artifacts(root, "0.1.5")
            home = root / "home"
            home.mkdir()
            cli.install(archive, manifest, home=home, platform=FakePlatform(), effective_uid=501)
            paths = cli._paths(home)
            same, _ = support_release_transport(self.payloads, "0.1.5")
            self.assertFalse(cli._automatic_support_update_offer(
                paths=paths, arguments=["version"], home=home,
                transport=same, effective_uid=501, now=100,
                interval=1, tty=lambda: True,
                prompt=lambda _: self.fail("no-change prompted"),
                output=lambda _: None, exec_launcher=None,
            ))
            no_change = cli._read_support_update_cache(paths)
            self.assertIsNotNone(no_change)
            assert no_change is not None
            self.assertEqual(no_change.latest_support_version, "0.1.5")

            candidate_bytes = cli.build_support_manifest_bytes(
                "0.1.6", "v0.1.6", self.payloads,
            )
            candidate = cli.parse_support_manifest(candidate_bytes)
            cli._publish_support_generation(
                paths, self.payloads, manifest_bytes=candidate_bytes,
            )
            current = cli._validate_owned_cli(paths)
            failed_state = cli.SupportState(
                "0.1.6", candidate.manifest_sha256,
                current.manifest.generation_id,
                (cli.FailedSupportGeneration(
                    candidate.generation_id, candidate.manifest_sha256,
                ),),
            )
            paths.support_state.write_bytes(cli.support_state_bytes(failed_state))
            paths.support_state.chmod(0o600)
            failed_check = cli.SupportUpdateCheck(
                200, "0.1.6", "v0.1.6", candidate.generation_id,
                candidate.manifest_sha256, False,
            )
            cli._write_support_update_cache(paths, failed_check)
            output: list[str] = []
            no_network = FakeReleaseTransport({})
            self.assertFalse(cli._automatic_support_update_offer(
                paths=paths, arguments=["version"], home=home,
                transport=no_network, effective_uid=501, now=201,
                interval=100, tty=lambda: True,
                prompt=lambda _: self.fail("failed target prompted"),
                output=output.append, exec_launcher=None,
            ))
            self.assertEqual(no_network.calls, [])
            self.assertTrue(any("already recorded as failed" in line for line in output))
            self.assertIsNone(cli._read_support_pending(paths))

    def test_support_cache_cannot_hide_downgrade_or_same_version_substitution(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest = write_artifacts(root, "0.1.5")
            home = root / "home"
            home.mkdir()
            cli.install(archive, manifest, home=home, platform=FakePlatform(), effective_uid=501)
            paths = cli._paths(home)

            substituted_payloads = dict(self.payloads)
            substituted_payloads[cli.SUPPORT_PAYLOAD_NAMES[1]] += b"\n# substituted\n"
            substitution, _ = support_release_transport(
                substituted_payloads, "0.1.5",
            )
            output: list[str] = []
            self.assertFalse(cli._automatic_support_update_offer(
                paths=paths, arguments=["version"], home=home,
                transport=substitution, effective_uid=501, now=10,
                interval=1, tty=lambda: False,
                prompt=lambda _: self.fail("substitution prompted"),
                output=output.append, exec_launcher=None,
            ))
            self.assertTrue(any("same-version" in line for line in output))
            self.assertIsNone(cli._read_support_update_cache(paths))

            downgrade, _ = support_release_transport(self.payloads, "0.1.4")
            output.clear()
            self.assertFalse(cli._automatic_support_update_offer(
                paths=paths, arguments=["version"], home=home,
                transport=downgrade, effective_uid=501, now=11,
                interval=1, tty=lambda: False,
                prompt=lambda _: self.fail("downgrade prompted"),
                output=output.append, exec_launcher=None,
            ))
            self.assertTrue(any("below the active" in line for line in output))
            self.assertIsNone(cli._read_support_update_cache(paths))

            active = cli._validate_owned_cli(paths).manifest
            forged = cli.SupportUpdateCheck(
                20, active.support_version, active.release_tag,
                active.generation_id, "f" * 64, False,
            )
            cli._write_support_update_cache(paths, forged)
            prior = paths.support_update_cache.read_bytes()
            output.clear()
            self.assertFalse(cli._automatic_support_update_offer(
                paths=paths, arguments=["version"], home=home,
                transport=FakeReleaseTransport({}), effective_uid=501,
                now=21, interval=100, tty=lambda: False,
                prompt=lambda _: self.fail("forged no-change prompted"),
                output=output.append, exec_launcher=None,
            ))
            self.assertTrue(any("does not bind the active" in line for line in output))
            self.assertEqual(paths.support_update_cache.read_bytes(), prior)

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
            real_stage = cli._write_private_staged
            real_exchange = cli._exchange_paths
            staged_identities: list[tuple[int, int]] = []
            exchange_events: list[
                tuple[tuple[int, int], tuple[int, int], tuple[int, int], tuple[int, int]]
            ] = []
            def record_real_stage(
                    path: Path, payload: bytes, label: str,
            ) -> tuple[int, int]:
                identity = real_stage(path, payload, label)
                if label == "support update-check cache":
                    staged_identities.append(identity)
                return identity
            def record_real_exchange(first: Path, second: Path) -> None:
                before = (cli._identity(first), cli._identity(second))
                real_exchange(first, second)
                exchange_events.append((
                    before[0], before[1], cli._identity(first), cli._identity(second),
                ))
            with mock.patch.object(
                    cli, "_write_private_staged", side_effect=record_real_stage,
            ), mock.patch.object(
                    cli, "_exchange_paths", side_effect=record_real_exchange,
            ):
                cli._write_support_update_cache(paths, check)
            self.assertEqual(len(staged_identities), 2)
            self.assertNotEqual(staged_identities[0], staged_identities[1])
            self.assertEqual(exchange_events, [(
                staged_identities[1], staged_identities[0],
                staged_identities[0], staged_identities[1],
            )])
            self.assertEqual(
                cli._identity(paths.support_update_cache), staged_identities[1],
            )
            self.assertEqual(paths.support_update_cache.stat().st_nlink, 1)
            self.assertEqual(paths.support_update_cache.stat().st_mode & 0o777, 0o600)
            self.assertEqual(cli._read_support_update_cache(paths), check)
            self.assertFalse(any(
                path.name.startswith(".preserved-support-update-cache-racer-")
                for path in paths.support.iterdir()
            ))
            self.assertEqual(tree_snapshot(paths.update_cache), app_cache_before)
            value = json.loads(paths.support_update_cache.read_bytes())
            self.assertEqual(set(value), cli.SUPPORT_UPDATE_CACHE_KEYS)
            self.assertTrue(value["declined"])
            paths.support_update_cache.chmod(0o644)
            with self.assertRaisesRegex(cli.DesktopCLIError, "ownership, mode"):
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

    def test_automatic_support_offer_non_tty_decline_cache_and_confirmed_stage(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest = write_artifacts(root, "0.1.5")
            home = root / "home"
            home.mkdir()
            cli.install(archive, manifest, home=home, platform=FakePlatform(), effective_uid=501)
            paths = cli._paths(home)

            transport, _ = support_release_transport(self.payloads)
            output: list[str] = []
            self.assertFalse(cli._automatic_support_update_offer(
                paths=paths, arguments=["version"], home=home,
                transport=transport, effective_uid=501, now=1000,
                interval=86400, tty=lambda: False,
                prompt=lambda _: self.fail("non-TTY must not prompt"),
                output=output.append, exec_launcher=None,
            ))
            cached = cli._read_support_update_cache(paths)
            self.assertIsNotNone(cached)
            assert cached is not None
            self.assertFalse(cached.declined)
            self.assertTrue(any("Support update available" in line for line in output))

            no_network = FakeReleaseTransport({})
            prompts: list[str] = []
            self.assertFalse(cli._automatic_support_update_offer(
                paths=paths, arguments=["version"], home=home,
                transport=no_network, effective_uid=501, now=1001,
                interval=86400, tty=lambda: True,
                prompt=lambda message: prompts.append(message) or "invalid",
                output=lambda _: None, exec_launcher=None,
            ))
            self.assertEqual(no_network.calls, [])
            self.assertEqual(len(prompts), 1)
            declined = cli._read_support_update_cache(paths)
            self.assertIsNotNone(declined)
            assert declined is not None
            self.assertTrue(declined.declined)

            # A declined fresh cache suppresses a repeat prompt and all transport.
            prompts.clear()
            self.assertFalse(cli._automatic_support_update_offer(
                paths=paths, arguments=["version"], home=home,
                transport=no_network, effective_uid=501, now=1002,
                interval=86400, tty=lambda: True,
                prompt=lambda message: prompts.append(message) or "yes",
                output=lambda _: None, exec_launcher=None,
            ))
            self.assertEqual(prompts, [])

            # A stale fresh check and only y/yes enters the same pending transaction.
            confirmed, _ = support_release_transport(self.payloads)
            executions: list[tuple[Path, list[str], dict[str, str]]] = []
            self.assertTrue(cli._automatic_support_update_offer(
                paths=paths, arguments=["version"], home=home,
                transport=confirmed, effective_uid=501, now=100000,
                interval=10, tty=lambda: True, prompt=lambda _: " YES ",
                output=lambda _: None,
                exec_launcher=lambda path, argv, env: executions.append((path, argv, env)),
            ))
            self.assertEqual(executions[0][1], [str(paths.launcher), "version"])
            self.assertIsNotNone(cli._read_support_pending(paths))

    def test_support_prompt_accepts_only_y_or_yes_and_every_other_answer_declines(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest = write_artifacts(root, "0.1.5")
            home = root / "home"
            home.mkdir()
            cli.install(archive, manifest, home=home, platform=FakePlatform(), effective_uid=501)
            paths = cli._paths(home)
            candidate = cli.parse_support_manifest(
                cli.build_support_manifest_bytes("0.1.6", "v0.1.6", self.payloads)
            )
            available = cli.SupportUpdateCheck(
                50, "0.1.6", "v0.1.6", candidate.generation_id,
                candidate.manifest_sha256, False,
            )
            for answer in ("", "n", "no", "invalid"):
                with self.subTest(decline=answer or "default"):
                    cli._write_support_update_cache(paths, available)
                    with mock.patch.object(cli, "stage_official_support_update") as stage:
                        self.assertFalse(cli._automatic_support_update_offer(
                            paths=paths, arguments=["version"], home=home,
                            transport=FakeReleaseTransport({}), effective_uid=501,
                            now=51, interval=100, tty=lambda: True,
                            prompt=lambda _, value=answer: value,
                            output=lambda _: None, exec_launcher=None,
                        ))
                    stage.assert_not_called()
                    declined = cli._read_support_update_cache(paths)
                    self.assertIsNotNone(declined)
                    assert declined is not None
                    self.assertTrue(declined.declined)
            for answer in ("y", " YES "):
                with self.subTest(affirmative=answer):
                    cli._write_support_update_cache(paths, available)
                    with mock.patch.object(
                            cli, "stage_official_support_update",
                            return_value=candidate.generation_id,
                    ) as stage:
                        self.assertTrue(cli._automatic_support_update_offer(
                            paths=paths, arguments=["version"], home=home,
                            transport=FakeReleaseTransport({}), effective_uid=501,
                            now=51, interval=100, tty=lambda: True,
                            prompt=lambda _, value=answer: value,
                            output=lambda _: None, exec_launcher=None,
                        ))
                    stage.assert_called_once()

    def test_corrupt_support_cache_warns_fail_open_without_support_network(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest = write_artifacts(root, "0.1.5")
            home = root / "home"
            home.mkdir()
            platform = FakePlatform()
            cli.install(archive, manifest, home=home, platform=platform, effective_uid=501)
            paths = cli._paths(home)
            cli._write_update_cache(paths, "0.1.5", 500)
            paths.support_update_cache.write_bytes(b"{}\n")
            paths.support_update_cache.chmod(0o600)
            output: list[str] = []
            transport = FakeReleaseTransport({})
            self.assertEqual(cli.run_installed(
                ["version"], home=home, platform=platform, transport=transport,
                effective_uid=501, clock=lambda: 501, tty=lambda: False,
                output=output.append,
            ), 0)
            self.assertEqual(transport.calls, [])
            self.assertTrue(any("Support update cache is invalid" in line for line in output))
            self.assertTrue(any("version: 0.1.5" in line for line in output))

    def test_explicit_support_failure_continues_app_and_reexec_guard_skips_once(self) -> None:
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
            from tests.test_desktop_user_cli import official_release_transport
            transport = official_release_transport(new_archive, new_manifest, "0.1.6")
            output: list[str] = []
            self.assertEqual(cli.run_installed(
                ["update"], home=home, platform=platform, transport=transport,
                effective_uid=501, output=output.append,
            ), 0)
            self.assertTrue(any("Support update failed" in line for line in output))
            self.assertTrue(any("App plane updated" in line for line in output))

            # Wrapper-derived one-shot guard performs no support request and allows App work.
            local_transport = FakeReleaseTransport({})
            self.assertEqual(cli.run_installed(
                ["update", "--archive", str(new_archive), "--manifest", str(new_manifest)],
                home=home, platform=platform, transport=local_transport,
                effective_uid=501, skip_support_check=True,
            ), 0)
            self.assertEqual(local_transport.calls, [])

    def test_support_release_producer_is_deterministic_exact_and_no_clobber(self) -> None:
        from scripts import support_release
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first, second = root / "first", root / "second"
            one = support_release.produce_support_release(first, "0.1.6")
            two = support_release.produce_support_release(second, "0.1.6")
            self.assertEqual(one, two)
            expected = {cli.SUPPORT_MANIFEST_NAME, *cli.SUPPORT_PAYLOAD_NAMES}
            self.assertEqual({path.name for path in first.iterdir()}, expected)
            self.assertEqual(
                {path.name: path.read_bytes() for path in first.iterdir()},
                {path.name: path.read_bytes() for path in second.iterdir()},
            )
            self.assertTrue(all((path.stat().st_mode & 0o777) == 0o600
                                for path in first.iterdir()))
            self.assertEqual(support_release.validate_support_release(first), one)
            before = tree_snapshot(first)
            with self.assertRaises(cli.DesktopCLIError):
                support_release.produce_support_release(first, "0.1.6")
            self.assertEqual(tree_snapshot(first), before)
            (first / "unexpected").write_bytes(b"x")
            with self.assertRaisesRegex(cli.DesktopCLIError, "file set"):
                support_release.validate_support_release(first)

    def test_failed_official_payload_validation_has_zero_managed_publication(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest = write_artifacts(root, "0.1.5")
            home = root / "home"
            home.mkdir()
            cli.install(archive, manifest, home=home, platform=FakePlatform(), effective_uid=501)
            paths = cli._paths(home)
            support_before = tree_snapshot(paths.support)
            transport, _ = support_release_transport(self.payloads)
            payload_url = next(
                url for url in transport.routes
                if "release-assets.githubusercontent.com" in url
                and transport.routes[url][2] == self.payloads[cli.SUPPORT_PAYLOAD_NAMES[0]]
            )
            status, headers, body = transport.routes[payload_url]
            transport.routes[payload_url] = (status, headers, b"X" + body[1:])
            with self.assertRaisesRegex(cli.DesktopCLIError, "SHA-256"):
                cli.stage_official_support_update(
                    home=home, transport=transport, effective_uid=501,
                    argv=[str(paths.launcher), "version"],
                    exec_launcher=lambda *_: self.fail("invalid payload executed"),
                )
            self.assertEqual(tree_snapshot(paths.support), support_before)
            self.assertFalse(paths.support_pending.exists())

    def test_support_cache_post_replace_failure_restores_exact_prior_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest = write_artifacts(root, "0.1.5")
            home = root / "home"
            home.mkdir()
            cli.install(archive, manifest, home=home, platform=FakePlatform(), effective_uid=501)
            paths = cli._paths(home)
            old_manifest = cli.parse_support_manifest(
                cli.build_support_manifest_bytes("0.1.6", "v0.1.6", self.payloads)
            )
            old = cli.SupportUpdateCheck(
                10, "0.1.6", "v0.1.6", old_manifest.generation_id,
                old_manifest.manifest_sha256, False,
            )
            cli._write_support_update_cache(paths, old)
            prior = paths.support_update_cache.read_bytes()
            identity = cli._identity(paths.support_update_cache)
            new_manifest = cli.parse_support_manifest(
                cli.build_support_manifest_bytes("0.1.7", "v0.1.7", self.payloads)
            )
            new = cli.SupportUpdateCheck(
                20, "0.1.7", "v0.1.7", new_manifest.generation_id,
                new_manifest.manifest_sha256, True,
            )
            real_fsync = cli._fsync_directory
            calls = 0
            def fail_once(path: Path) -> None:
                nonlocal calls
                calls += 1
                if calls == 1:
                    raise cli.DesktopCLIError("injected cache directory fsync failure")
                real_fsync(path)
            with mock.patch.object(cli, "_fsync_directory", side_effect=fail_once):
                with self.assertRaisesRegex(cli.DesktopCLIError, "injected"):
                    cli._write_support_update_cache(paths, new)
            self.assertEqual(paths.support_update_cache.read_bytes(), prior)
            self.assertEqual(cli._identity(paths.support_update_cache), identity)
            self.assertEqual(cli._read_support_update_cache(paths), old)
            self.assertFalse(any(
                path.name.startswith(".preserved-support-update-cache-racer-")
                for path in paths.support.iterdir()
            ))

    def test_support_cache_final_exchange_race_preserves_racer_and_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest = write_artifacts(root, "0.1.5")
            home = root / "home"
            home.mkdir()
            cli.install(archive, manifest, home=home, platform=FakePlatform(), effective_uid=501)
            paths = cli._paths(home)
            first_manifest = cli.parse_support_manifest(
                cli.build_support_manifest_bytes("0.1.6", "v0.1.6", self.payloads)
            )
            second_manifest = cli.parse_support_manifest(
                cli.build_support_manifest_bytes("0.1.7", "v0.1.7", self.payloads)
            )
            first = cli.SupportUpdateCheck(
                1, "0.1.6", "v0.1.6", first_manifest.generation_id,
                first_manifest.manifest_sha256, False,
            )
            second = cli.SupportUpdateCheck(
                2, "0.1.7", "v0.1.7", second_manifest.generation_id,
                second_manifest.manifest_sha256, False,
            )
            cli._write_support_update_cache(paths, first)
            racer = paths.support / ".test-racer"
            racer_bytes = cli._support_update_cache_bytes(
                dataclasses.replace(first, checked_at=99, declined=True)
            )
            racer.write_bytes(racer_bytes)
            racer.chmod(0o600)
            real_exchange = cli._exchange_paths
            raced = False
            def exchange(first_path: Path, second_path: Path) -> None:
                nonlocal raced
                if not raced and second_path == paths.support_update_cache:
                    raced = True
                    os.replace(racer, paths.support_update_cache)
                real_exchange(first_path, second_path)
            with mock.patch.object(cli, "_exchange_paths", side_effect=exchange):
                with self.assertRaisesRegex(cli.DesktopCLIError, "raced"):
                    cli._write_support_update_cache(paths, second)
            self.assertEqual(paths.support_update_cache.read_bytes(), racer_bytes)
            self.assertEqual(
                {path.name for path in paths.support.iterdir()
                 if path.name.startswith(".update-check-")
                 or path.name.startswith(".preserved-support-update-cache-racer-")},
                set(),
            )

    def test_support_cache_post_read_late_race_restores_prior_and_preserves_racer(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest = write_artifacts(root, "0.1.5")
            home = root / "home"
            home.mkdir()
            cli.install(archive, manifest, home=home, platform=FakePlatform(), effective_uid=501)
            paths = cli._paths(home)

            old_manifest = cli.parse_support_manifest(
                cli.build_support_manifest_bytes("0.1.6", "v0.1.6", self.payloads)
            )
            new_manifest = cli.parse_support_manifest(
                cli.build_support_manifest_bytes("0.1.7", "v0.1.7", self.payloads)
            )
            old = cli.SupportUpdateCheck(
                10, "0.1.6", "v0.1.6", old_manifest.generation_id,
                old_manifest.manifest_sha256, False,
            )
            new = cli.SupportUpdateCheck(
                20, "0.1.7", "v0.1.7", new_manifest.generation_id,
                new_manifest.manifest_sha256, True,
            )
            cli._write_support_update_cache(paths, old)
            self.assertEqual(cli._read_support_update_cache(paths), old)
            prior_bytes = paths.support_update_cache.read_bytes()
            prior_identity = cli._identity(paths.support_update_cache)
            staged_bytes = cli._support_update_cache_bytes(new)
            self.assertEqual(set(json.loads(staged_bytes)), cli.SUPPORT_UPDATE_CACHE_KEYS)

            racer = paths.support / ".test-post-read-racer"
            racer_check = dataclasses.replace(old, checked_at=99, declined=True)
            racer_bytes = cli._support_update_cache_bytes(racer_check)
            racer.write_bytes(racer_bytes)
            racer.chmod(0o600)
            racer_identity = cli._identity(racer)

            real_read = cli._read_managed_support_file
            target_reads = 0
            raced = False
            published_bytes: bytes | None = None
            published_identity: tuple[int, int] | None = None

            def read_then_race(
                    path: Path, label: str, maximum_bytes: int,
                    expected_size: int | None = None,
                    expected_mode: int = cli.SUPPORT_PAYLOAD_MODE,
            ) -> tuple[bytes, tuple[int, int]]:
                nonlocal target_reads, raced, published_bytes, published_identity
                result = real_read(
                    path, label, maximum_bytes, expected_size, expected_mode,
                )
                if path == paths.support_update_cache:
                    target_reads += 1
                    if target_reads == 2:
                        published_bytes, published_identity = result
                        os.replace(racer, paths.support_update_cache)
                        raced = True
                return result

            error: cli.DesktopCLIError | None = None
            with mock.patch.object(
                    cli, "_read_managed_support_file", side_effect=read_then_race,
            ):
                try:
                    cli._write_support_update_cache(paths, new)
                except cli.DesktopCLIError as caught:
                    error = caught

            prior_locations = [
                path for path in paths.support.iterdir()
                if cli._matches_identity(path, prior_identity)
            ]
            canonical_is_racer = cli._matches_identity(
                paths.support_update_cache, racer_identity,
            )
            self.assertIsNotNone(
                error,
                "publication returned success after the post-read replacement; "
                f"target_reads={target_reads} raced={raced} "
                f"canonical_is_racer={canonical_is_racer} "
                f"prior_locations={[path.name for path in prior_locations]}",
            )
            assert error is not None
            self.assertRegex(str(error), "publication was replaced|raced")
            self.assertTrue(raced)
            self.assertEqual(target_reads, 2)
            self.assertEqual(published_bytes, staged_bytes)
            self.assertIsNotNone(published_identity)
            assert published_identity is not None
            self.assertEqual(cli._identity(paths.support_update_cache), prior_identity)
            self.assertEqual(paths.support_update_cache.read_bytes(), prior_bytes)
            self.assertEqual(cli._read_support_update_cache(paths), old)
            self.assertFalse(racer.exists())

            preserved = [
                path for path in paths.support.iterdir()
                if path.name.startswith(".preserved-support-update-cache-racer-")
            ]
            self.assertEqual(len(preserved), 1)
            self.assertEqual(cli._identity(preserved[0]), racer_identity)
            self.assertEqual(preserved[0].read_bytes(), racer_bytes)
            self.assertEqual(preserved[0].stat().st_nlink, 1)
            self.assertEqual(preserved[0].stat().st_mode & 0o777, 0o600)
            self.assertFalse(any(
                cli._matches_identity(path, published_identity)
                for path in paths.support.iterdir()
            ))
            self.assertEqual(
                [path.name for path in paths.support.iterdir()
                 if path.name.startswith(".update-check-")],
                [],
            )

    def test_absent_support_cache_post_read_late_racer_is_refused_without_clobber(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest = write_artifacts(root, "0.1.5")
            home = root / "home"
            home.mkdir()
            cli.install(archive, manifest, home=home, platform=FakePlatform(), effective_uid=501)
            paths = cli._paths(home)
            self.assertFalse(paths.support_update_cache.exists())

            manifest_value = cli.parse_support_manifest(
                cli.build_support_manifest_bytes("0.1.6", "v0.1.6", self.payloads)
            )
            new = cli.SupportUpdateCheck(
                20, "0.1.6", "v0.1.6", manifest_value.generation_id,
                manifest_value.manifest_sha256, False,
            )
            staged_bytes = cli._support_update_cache_bytes(new)
            self.assertEqual(set(json.loads(staged_bytes)), cli.SUPPORT_UPDATE_CACHE_KEYS)
            racer = paths.support / ".test-absent-post-read-racer"
            racer_check = dataclasses.replace(new, checked_at=99, declined=True)
            racer_bytes = cli._support_update_cache_bytes(racer_check)
            racer.write_bytes(racer_bytes)
            racer.chmod(0o600)
            racer_identity = cli._identity(racer)

            real_read = cli._read_managed_support_file
            target_reads = 0
            raced = False
            published_identity: tuple[int, int] | None = None

            def read_then_race(
                    path: Path, label: str, maximum_bytes: int,
                    expected_size: int | None = None,
                    expected_mode: int = cli.SUPPORT_PAYLOAD_MODE,
            ) -> tuple[bytes, tuple[int, int]]:
                nonlocal target_reads, raced, published_identity
                result = real_read(
                    path, label, maximum_bytes, expected_size, expected_mode,
                )
                if path == paths.support_update_cache:
                    target_reads += 1
                    if target_reads == 1:
                        self.assertEqual(result[0], staged_bytes)
                        published_identity = result[1]
                        os.replace(racer, paths.support_update_cache)
                        raced = True
                return result

            error: cli.DesktopCLIError | None = None
            with mock.patch.object(
                    cli, "_read_managed_support_file", side_effect=read_then_race,
            ):
                try:
                    cli._write_support_update_cache(paths, new)
                except cli.DesktopCLIError as caught:
                    error = caught

            self.assertIsNotNone(
                error,
                "initially absent publication returned success after the post-read "
                f"replacement; target_reads={target_reads} raced={raced} "
                f"canonical_is_racer={cli._matches_identity(paths.support_update_cache, racer_identity)}",
            )
            assert error is not None
            self.assertRegex(str(error), "publication was replaced|raced")
            self.assertTrue(raced)
            self.assertEqual(target_reads, 1)
            self.assertFalse(racer.exists())
            self.assertEqual(cli._identity(paths.support_update_cache), racer_identity)
            self.assertEqual(paths.support_update_cache.read_bytes(), racer_bytes)
            self.assertEqual(cli._read_support_update_cache(paths), racer_check)
            self.assertIsNotNone(published_identity)
            assert published_identity is not None
            self.assertFalse(any(
                cli._matches_identity(path, published_identity)
                for path in paths.support.iterdir()
            ))
            self.assertEqual(
                [path.name for path in paths.support.iterdir()
                 if path.name.startswith(".update-check-")
                 or path.name.startswith(".preserved-support-update-cache-racer-")],
                [],
            )

    def test_support_cache_post_final_check_race_restores_prior_and_preserves_racer(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest = write_artifacts(root, "0.1.5")
            home = root / "home"
            home.mkdir()
            cli.install(archive, manifest, home=home, platform=FakePlatform(), effective_uid=501)
            paths = cli._paths(home)

            old_manifest = cli.parse_support_manifest(
                cli.build_support_manifest_bytes("0.1.6", "v0.1.6", self.payloads)
            )
            new_manifest = cli.parse_support_manifest(
                cli.build_support_manifest_bytes("0.1.7", "v0.1.7", self.payloads)
            )
            old = cli.SupportUpdateCheck(
                10, "0.1.6", "v0.1.6", old_manifest.generation_id,
                old_manifest.manifest_sha256, False,
            )
            new = cli.SupportUpdateCheck(
                20, "0.1.7", "v0.1.7", new_manifest.generation_id,
                new_manifest.manifest_sha256, True,
            )
            cli._write_support_update_cache(paths, old)
            prior_bytes = paths.support_update_cache.read_bytes()
            prior_identity = cli._identity(paths.support_update_cache)
            prior_facts = paths.support_update_cache.stat()
            self.assertEqual(prior_facts.st_mode & 0o777, 0o600)
            self.assertEqual(prior_facts.st_nlink, 1)

            racer = paths.support / ".test-post-final-check-racer"
            racer_check = dataclasses.replace(old, checked_at=99, declined=True)
            racer_bytes = cli._support_update_cache_bytes(racer_check)
            racer.write_bytes(racer_bytes)
            racer.chmod(0o600)
            racer_identity = cli._identity(racer)

            real_stage = cli._write_private_staged
            real_matches = cli._matches_identity
            staged_identities: list[tuple[int, int]] = []
            canonical_staged_checks = 0
            triggered = False

            def record_real_stage(path: Path, payload: bytes, label: str) -> tuple[int, int]:
                identity = real_stage(path, payload, label)
                if label == "support update-check cache":
                    staged_identities.append(identity)
                return identity

            def match_then_race(path: Path, expected: tuple[int, int]) -> bool:
                nonlocal canonical_staged_checks, triggered
                result = real_matches(path, expected)
                if (path == paths.support_update_cache and result
                        and expected in staged_identities):
                    canonical_staged_checks += 1
                    if canonical_staged_checks == 2 and not triggered:
                        os.replace(racer, paths.support_update_cache)
                        triggered = True
                return result

            error: cli.DesktopCLIError | None = None
            with mock.patch.object(
                    cli, "_write_private_staged", side_effect=record_real_stage,
            ), mock.patch.object(cli, "_matches_identity", side_effect=match_then_race):
                try:
                    cli._write_support_update_cache(paths, new)
                except cli.DesktopCLIError as caught:
                    error = caught

            leaves = [
                path for path in paths.support.iterdir()
                if path.name.startswith(".update-check-")
                or path.name.startswith(".preserved-support-update-cache-racer-")
            ]
            outcome = {
                "triggered": triggered,
                "canonical_staged_checks": canonical_staged_checks,
                "returned_success": error is None,
                "canonical_exists": paths.support_update_cache.exists(),
                "canonical_is_prior": real_matches(
                    paths.support_update_cache, prior_identity,
                ),
                "canonical_is_racer": real_matches(
                    paths.support_update_cache, racer_identity,
                ),
                "prior_locations": [
                    path.name for path in paths.support.iterdir()
                    if real_matches(path, prior_identity)
                ],
                "leaves": [path.name for path in leaves],
            }
            self.assertIsNotNone(error, json.dumps(outcome, sort_keys=True))
            assert error is not None
            self.assertRegex(str(error), "publication was replaced|raced")
            self.assertTrue(triggered)
            self.assertGreaterEqual(canonical_staged_checks, 2)
            self.assertEqual(cli._identity(paths.support_update_cache), prior_identity)
            self.assertEqual(paths.support_update_cache.read_bytes(), prior_bytes)
            self.assertEqual(cli._read_support_update_cache(paths), old)
            restored_facts = paths.support_update_cache.stat()
            self.assertEqual(restored_facts.st_mode & 0o777, 0o600)
            self.assertEqual(restored_facts.st_nlink, 1)
            self.assertFalse(racer.exists())
            preserved = [
                path for path in leaves
                if real_matches(path, racer_identity)
            ]
            self.assertEqual(len(preserved), 1, json.dumps(outcome, sort_keys=True))
            self.assertEqual(preserved[0].read_bytes(), racer_bytes)
            self.assertEqual(preserved[0].stat().st_mode & 0o777, 0o600)
            self.assertEqual(preserved[0].stat().st_nlink, 1)
            self.assertFalse(any(
                real_matches(path, identity)
                for path in paths.support.iterdir()
                for identity in staged_identities
            ))
            self.assertEqual(leaves, preserved)

    def test_absent_support_cache_post_final_check_racer_is_refused_without_clobber(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest = write_artifacts(root, "0.1.5")
            home = root / "home"
            home.mkdir()
            cli.install(archive, manifest, home=home, platform=FakePlatform(), effective_uid=501)
            paths = cli._paths(home)
            self.assertFalse(paths.support_update_cache.exists())

            manifest_value = cli.parse_support_manifest(
                cli.build_support_manifest_bytes("0.1.6", "v0.1.6", self.payloads)
            )
            new = cli.SupportUpdateCheck(
                20, "0.1.6", "v0.1.6", manifest_value.generation_id,
                manifest_value.manifest_sha256, False,
            )
            racer = paths.support / ".test-absent-post-final-check-racer"
            racer_check = dataclasses.replace(new, checked_at=99, declined=True)
            racer_bytes = cli._support_update_cache_bytes(racer_check)
            racer.write_bytes(racer_bytes)
            racer.chmod(0o600)
            racer_identity = cli._identity(racer)

            real_stage = cli._write_private_staged
            real_matches = cli._matches_identity
            staged_identities: list[tuple[int, int]] = []
            canonical_staged_checks = 0
            triggered = False

            def record_real_stage(path: Path, payload: bytes, label: str) -> tuple[int, int]:
                identity = real_stage(path, payload, label)
                if label == "support update-check cache":
                    staged_identities.append(identity)
                return identity

            def match_then_race(path: Path, expected: tuple[int, int]) -> bool:
                nonlocal canonical_staged_checks, triggered
                result = real_matches(path, expected)
                if (path == paths.support_update_cache and result
                        and expected in staged_identities):
                    canonical_staged_checks += 1
                    if canonical_staged_checks == 1 and not triggered:
                        os.replace(racer, paths.support_update_cache)
                        triggered = True
                return result

            error: cli.DesktopCLIError | None = None
            with mock.patch.object(
                    cli, "_write_private_staged", side_effect=record_real_stage,
            ), mock.patch.object(cli, "_matches_identity", side_effect=match_then_race):
                try:
                    cli._write_support_update_cache(paths, new)
                except cli.DesktopCLIError as caught:
                    error = caught

            leaves = [
                path for path in paths.support.iterdir()
                if path.name.startswith(".update-check-")
                or path.name.startswith(".preserved-support-update-cache-racer-")
            ]
            outcome = {
                "triggered": triggered,
                "canonical_staged_checks": canonical_staged_checks,
                "returned_success": error is None,
                "canonical_exists": paths.support_update_cache.exists(),
                "canonical_is_racer": real_matches(
                    paths.support_update_cache, racer_identity,
                ),
                "leaves": [path.name for path in leaves],
            }
            self.assertIsNotNone(error, json.dumps(outcome, sort_keys=True))
            assert error is not None
            self.assertRegex(str(error), "publication was replaced|raced")
            self.assertTrue(triggered)
            self.assertGreaterEqual(canonical_staged_checks, 1)
            self.assertFalse(racer.exists())
            self.assertEqual(cli._identity(paths.support_update_cache), racer_identity)
            self.assertEqual(paths.support_update_cache.read_bytes(), racer_bytes)
            self.assertEqual(cli._read_support_update_cache(paths), racer_check)
            racer_facts = paths.support_update_cache.stat()
            self.assertEqual(racer_facts.st_mode & 0o777, 0o600)
            self.assertEqual(racer_facts.st_nlink, 1)
            self.assertFalse(any(
                real_matches(path, identity)
                for path in paths.support.iterdir()
                for identity in staged_identities
            ))
            self.assertEqual(leaves, [])

    def test_support_cache_restore_recovers_when_detected_racer_disappears(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest = write_artifacts(root, "0.1.5")
            home = root / "home"
            home.mkdir()
            cli.install(archive, manifest, home=home, platform=FakePlatform(), effective_uid=501)
            paths = cli._paths(home)

            old_manifest = cli.parse_support_manifest(
                cli.build_support_manifest_bytes("0.1.6", "v0.1.6", self.payloads)
            )
            new_manifest = cli.parse_support_manifest(
                cli.build_support_manifest_bytes("0.1.7", "v0.1.7", self.payloads)
            )
            old = cli.SupportUpdateCheck(
                10, "0.1.6", "v0.1.6", old_manifest.generation_id,
                old_manifest.manifest_sha256, False,
            )
            new = cli.SupportUpdateCheck(
                20, "0.1.7", "v0.1.7", new_manifest.generation_id,
                new_manifest.manifest_sha256, True,
            )
            cli._write_support_update_cache(paths, old)
            prior_bytes = paths.support_update_cache.read_bytes()
            prior_identity = cli._identity(paths.support_update_cache)

            racer = paths.support / ".test-disappearing-restoration-racer"
            racer_check = dataclasses.replace(old, checked_at=99, declined=True)
            racer_bytes = cli._support_update_cache_bytes(racer_check)
            racer.write_bytes(racer_bytes)
            racer.chmod(0o600)
            racer_identity = cli._identity(racer)
            reappearing = paths.support / ".test-reappearing-restoration-racer"
            reappearing_check = dataclasses.replace(old, checked_at=199, declined=False)
            reappearing_bytes = cli._support_update_cache_bytes(reappearing_check)
            reappearing.write_bytes(reappearing_bytes)
            reappearing.chmod(0o600)
            reappearing_identity = cli._identity(reappearing)

            real_read = cli._read_managed_support_file
            real_exchange = cli._exchange_paths
            real_link = os.link
            real_matches = cli._matches_identity
            real_stage = cli._write_private_staged
            staged_identities: list[tuple[int, int]] = []
            target_reads = 0
            post_read_replaced = False
            restore_identity_captured: tuple[int, int] | None = None
            disappeared_before_restore = False
            reappeared_during_recovery = False
            exchange_count = 0

            def record_real_stage(path: Path, payload: bytes, label: str) -> tuple[int, int]:
                identity = real_stage(path, payload, label)
                if label == "support update-check cache":
                    staged_identities.append(identity)
                return identity

            def read_then_race(
                    path: Path, label: str, maximum_bytes: int,
                    expected_size: int | None = None,
                    expected_mode: int = cli.SUPPORT_PAYLOAD_MODE,
            ) -> tuple[bytes, tuple[int, int]]:
                nonlocal target_reads, post_read_replaced
                result = real_read(
                    path, label, maximum_bytes, expected_size, expected_mode,
                )
                if path == paths.support_update_cache:
                    target_reads += 1
                    if target_reads == 2:
                        os.replace(racer, paths.support_update_cache)
                        post_read_replaced = True
                return result

            def exchange_then_disappear(first: Path, second: Path) -> None:
                nonlocal exchange_count, restore_identity_captured
                nonlocal disappeared_before_restore
                exchange_count += 1
                if (second == paths.support_update_cache
                        and real_matches(first, prior_identity)
                        and not disappeared_before_restore):
                    restore_identity_captured = cli._identity(paths.support_update_cache)
                    paths.support_update_cache.unlink()
                    disappeared_before_restore = True
                real_exchange(first, second)

            def link_then_reappear(
                    source: Path, destination: Path, *,
                    follow_symlinks: bool = True,
            ) -> None:
                nonlocal reappeared_during_recovery
                if (source.parent == paths.support
                        and destination == paths.support_update_cache
                        and real_matches(source, prior_identity)
                        and disappeared_before_restore
                        and not reappeared_during_recovery):
                    os.replace(reappearing, paths.support_update_cache)
                    reappeared_during_recovery = True
                real_link(
                    source, destination, follow_symlinks=follow_symlinks,
                )

            error: cli.DesktopCLIError | None = None
            with mock.patch.object(
                    cli, "_write_private_staged", side_effect=record_real_stage,
            ), mock.patch.object(
                    cli, "_read_managed_support_file", side_effect=read_then_race,
            ), mock.patch.object(
                    cli, "_exchange_paths", side_effect=exchange_then_disappear,
            ), mock.patch.object(
                    cli.os, "link", side_effect=link_then_reappear,
            ):
                try:
                    cli._write_support_update_cache(paths, new)
                except cli.DesktopCLIError as caught:
                    error = caught

            leaves = [
                path for path in paths.support.iterdir()
                if path.name.startswith(".update-check-")
                or path.name.startswith(".preserved-support-update-cache-racer-")
            ]
            outcome = {
                "post_read_replaced": post_read_replaced,
                "disappeared_before_restore": disappeared_before_restore,
                "reappeared_during_recovery": reappeared_during_recovery,
                "restore_identity_was_racer": restore_identity_captured == racer_identity,
                "exchange_count": exchange_count,
                "returned_success": error is None,
                "canonical_exists": paths.support_update_cache.exists(),
                "canonical_is_prior": real_matches(
                    paths.support_update_cache, prior_identity,
                ),
                "prior_locations": [
                    path.name for path in paths.support.iterdir()
                    if real_matches(path, prior_identity)
                ],
                "leaves": [path.name for path in leaves],
            }
            self.assertIsNotNone(error, json.dumps(outcome, sort_keys=True))
            self.assertTrue(post_read_replaced, json.dumps(outcome, sort_keys=True))
            self.assertTrue(disappeared_before_restore, json.dumps(outcome, sort_keys=True))
            self.assertTrue(reappeared_during_recovery, json.dumps(outcome, sort_keys=True))
            self.assertEqual(restore_identity_captured, racer_identity)
            self.assertTrue(
                paths.support_update_cache.exists(), json.dumps(outcome, sort_keys=True),
            )
            self.assertEqual(cli._identity(paths.support_update_cache), prior_identity)
            self.assertEqual(paths.support_update_cache.read_bytes(), prior_bytes)
            self.assertEqual(cli._read_support_update_cache(paths), old)
            restored_facts = paths.support_update_cache.stat()
            self.assertEqual(restored_facts.st_mode & 0o777, 0o600)
            self.assertEqual(restored_facts.st_nlink, 1)
            self.assertFalse(racer.exists())
            self.assertFalse(reappearing.exists())
            self.assertFalse(any(
                real_matches(path, identity)
                for path in paths.support.iterdir()
                for identity in staged_identities
            ))
            preserved = [
                path for path in leaves
                if real_matches(path, reappearing_identity)
            ]
            self.assertEqual(len(preserved), 1, json.dumps(outcome, sort_keys=True))
            self.assertEqual(preserved[0].read_bytes(), reappearing_bytes)
            self.assertEqual(preserved[0].stat().st_mode & 0o777, 0o600)
            self.assertEqual(preserved[0].stat().st_nlink, 1)
            self.assertEqual(leaves, preserved)

    def _installed_cache_pair(
            self, root: Path,
    ) -> tuple[cli.ManagedPaths, cli.SupportUpdateCheck, cli.SupportUpdateCheck]:
        archive, manifest = write_artifacts(root, "0.1.5")
        home = root / "home"
        home.mkdir()
        cli.install(
            archive, manifest, home=home, platform=FakePlatform(), effective_uid=501,
        )
        paths = cli._paths(home)
        old_manifest = cli.parse_support_manifest(
            cli.build_support_manifest_bytes("0.1.6", "v0.1.6", self.payloads)
        )
        new_manifest = cli.parse_support_manifest(
            cli.build_support_manifest_bytes("0.1.7", "v0.1.7", self.payloads)
        )
        old = cli.SupportUpdateCheck(
            10, "0.1.6", "v0.1.6", old_manifest.generation_id,
            old_manifest.manifest_sha256, False,
        )
        new = cli.SupportUpdateCheck(
            20, "0.1.7", "v0.1.7", new_manifest.generation_id,
            new_manifest.manifest_sha256, True,
        )
        cli._write_support_update_cache(paths, old)
        return paths, old, new

    def _cache_transaction_entries(self, paths: cli.ManagedPaths) -> list[Path]:
        return [
            path for path in paths.support.iterdir()
            if path.name.startswith(".support-update-cache-txn-")
            or path.name.startswith(".preserved-support-update-cache-racer-")
        ]

    def _assert_exact_cache(
            self, path: Path, payload: bytes, identity: tuple[int, int],
    ) -> None:
        facts = path.lstat()
        self.assertTrue(path.is_file())
        self.assertFalse(path.is_symlink())
        self.assertEqual((facts.st_dev, facts.st_ino), identity)
        self.assertEqual(path.read_bytes(), payload)
        self.assertEqual(facts.st_mode & 0o777, 0o600)
        self.assertEqual(facts.st_nlink, 1)

    def test_support_cache_prior_bytes_and_identity_share_one_descriptor_observation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            paths, old, new = self._installed_cache_pair(root)
            prior_bytes = paths.support_update_cache.read_bytes()
            prior_identity = cli._identity(paths.support_update_cache)
            foreign = paths.support / ".test-descriptor-observation-racer"
            foreign_check = dataclasses.replace(old, checked_at=99, declined=True)
            foreign_bytes = cli._support_update_cache_bytes(foreign_check)
            foreign.write_bytes(foreign_bytes)
            foreign.chmod(0o600)
            foreign_identity = cli._identity(foreign)

            real_read = cli._read_managed_support_file
            replaced_after_read = False

            def read_then_replace(
                    path: Path, label: str, maximum_bytes: int,
                    expected_size: int | None = None,
                    expected_mode: int = cli.SUPPORT_PAYLOAD_MODE,
            ) -> tuple[bytes, tuple[int, int]]:
                nonlocal replaced_after_read
                result = real_read(
                    path, label, maximum_bytes, expected_size, expected_mode,
                )
                if path == paths.support_update_cache and not replaced_after_read:
                    self.assertEqual(result, (prior_bytes, prior_identity))
                    os.replace(foreign, paths.support_update_cache)
                    replaced_after_read = True
                return result

            caught: BaseException | None = None
            with mock.patch.object(
                    cli, "_read_managed_support_file", side_effect=read_then_replace,
            ):
                try:
                    cli._write_support_update_cache(paths, new)
                except BaseException as error:
                    caught = error

            self.assertTrue(replaced_after_read)
            self.assertIsInstance(caught, cli.DesktopCLIError)
            self._assert_exact_cache(
                paths.support_update_cache, foreign_bytes, foreign_identity,
            )
            self.assertEqual(cli._read_support_update_cache(paths), foreign_check)
            self.assertFalse(any(
                cli._matches_identity(path, prior_identity)
                for path in paths.support.iterdir()
            ))
            self.assertEqual(self._cache_transaction_entries(paths), [])

    def test_first_support_cache_stage_readback_failure_cleans_private_transaction(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            paths, old, new = self._installed_cache_pair(Path(temporary))
            prior_bytes = paths.support_update_cache.read_bytes()
            prior_identity = cli._identity(paths.support_update_cache)
            real_read = cli._read_managed_support_file
            stage_reads = 0

            def fail_first_stage_read(
                    path: Path, label: str, maximum_bytes: int,
                    expected_size: int | None = None,
                    expected_mode: int = cli.SUPPORT_PAYLOAD_MODE,
            ) -> tuple[bytes, tuple[int, int]]:
                nonlocal stage_reads
                if label == "staged support update-check cache":
                    stage_reads += 1
                    if stage_reads == 1:
                        raise cli.DesktopCLIError("injected first stage readback failure")
                return real_read(
                    path, label, maximum_bytes, expected_size, expected_mode,
                )

            with mock.patch.object(
                    cli, "_read_managed_support_file", side_effect=fail_first_stage_read,
            ):
                with self.assertRaisesRegex(
                        cli.DesktopCLIError, "injected first stage readback failure"):
                    cli._write_support_update_cache(paths, new)
            self.assertEqual(stage_reads, 1)
            self._assert_exact_cache(
                paths.support_update_cache, prior_bytes, prior_identity,
            )
            self.assertEqual(cli._read_support_update_cache(paths), old)
            self.assertEqual(self._cache_transaction_entries(paths), [])

    def test_second_support_cache_stage_readback_failure_cleans_private_transaction(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            paths, old, new = self._installed_cache_pair(Path(temporary))
            prior_bytes = paths.support_update_cache.read_bytes()
            prior_identity = cli._identity(paths.support_update_cache)
            real_read = cli._read_managed_support_file
            stage_reads = 0

            def fail_second_stage_read(
                    path: Path, label: str, maximum_bytes: int,
                    expected_size: int | None = None,
                    expected_mode: int = cli.SUPPORT_PAYLOAD_MODE,
            ) -> tuple[bytes, tuple[int, int]]:
                nonlocal stage_reads
                if label == "staged support update-check cache":
                    stage_reads += 1
                    if stage_reads == 2:
                        raise cli.DesktopCLIError("injected second stage readback failure")
                return real_read(
                    path, label, maximum_bytes, expected_size, expected_mode,
                )

            with mock.patch.object(
                    cli, "_read_managed_support_file", side_effect=fail_second_stage_read,
            ):
                with self.assertRaisesRegex(
                        cli.DesktopCLIError, "injected second stage readback failure"):
                    cli._write_support_update_cache(paths, new)
            self.assertEqual(stage_reads, 2)
            self._assert_exact_cache(
                paths.support_update_cache, prior_bytes, prior_identity,
            )
            self.assertEqual(cli._read_support_update_cache(paths), old)
            self.assertEqual(self._cache_transaction_entries(paths), [])

    def test_absent_support_cache_never_unlinks_a_flat_private_stage_leaf(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest = write_artifacts(root, "0.1.5")
            home = root / "home"
            home.mkdir()
            cli.install(
                archive, manifest, home=home, platform=FakePlatform(), effective_uid=501,
            )
            paths = cli._paths(home)
            self.assertFalse(paths.support_update_cache.exists())
            candidate_manifest = cli.parse_support_manifest(
                cli.build_support_manifest_bytes("0.1.6", "v0.1.6", self.payloads)
            )
            candidate = cli.SupportUpdateCheck(
                20, "0.1.6", "v0.1.6", candidate_manifest.generation_id,
                candidate_manifest.manifest_sha256, False,
            )
            candidate_bytes = cli._support_update_cache_bytes(candidate)
            real_unlink = Path.unlink
            flat_stage_unlink = False

            def refuse_flat_stage_unlink(path: Path, *args, **kwargs) -> None:
                nonlocal flat_stage_unlink
                if (path.parent == paths.support
                        and path.name.startswith(
                            ".preserved-support-update-cache-racer-"
                        )
                        and path.name.endswith("-rollback")):
                    flat_stage_unlink = True
                    raise PermissionError("injected support-root private-leaf unlink refusal")
                real_unlink(path, *args, **kwargs)

            caught: BaseException | None = None
            with mock.patch.object(Path, "unlink", new=refuse_flat_stage_unlink):
                try:
                    cli._write_support_update_cache(paths, candidate)
                except BaseException as error:
                    caught = error
            self.assertIsNone(caught, f"unexpected publication failure: {caught}")
            self.assertFalse(flat_stage_unlink)
            self._assert_exact_cache(
                paths.support_update_cache, candidate_bytes,
                cli._identity(paths.support_update_cache),
            )
            self.assertEqual(cli._read_support_update_cache(paths), candidate)
            self.assertEqual(self._cache_transaction_entries(paths), [])

    def test_support_cache_precommit_publication_fsync_failure_restores_prior(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            paths, old, new = self._installed_cache_pair(Path(temporary))
            prior_bytes = paths.support_update_cache.read_bytes()
            prior_identity = cli._identity(paths.support_update_cache)
            real_exchange = cli._exchange_paths
            real_fsync = cli._fsync_directory
            final_exchange = False
            injected = False

            def record_final_exchange(first: Path, second: Path) -> None:
                nonlocal final_exchange
                real_exchange(first, second)
                if (second == paths.support_update_cache
                        and (first.name == "commit" or first.name.endswith("-commit"))):
                    final_exchange = True

            def fail_publication_fsync(path: Path) -> None:
                nonlocal injected
                if final_exchange and path == paths.support and not injected:
                    injected = True
                    raise cli.DesktopCLIError(
                        "injected pre-commit publication directory fsync failure"
                    )
                real_fsync(path)

            with mock.patch.object(
                    cli, "_exchange_paths", side_effect=record_final_exchange,
            ), mock.patch.object(
                    cli, "_fsync_directory", side_effect=fail_publication_fsync,
            ):
                with self.assertRaisesRegex(
                        cli.DesktopCLIError, "injected pre-commit"):
                    cli._write_support_update_cache(paths, new)
            self.assertTrue(final_exchange)
            self.assertTrue(injected)
            self._assert_exact_cache(
                paths.support_update_cache, prior_bytes, prior_identity,
            )
            self.assertEqual(cli._read_support_update_cache(paths), old)
            self.assertEqual(self._cache_transaction_entries(paths), [])

    def test_support_cache_postcommit_cleanup_fsync_is_a_committed_diagnostic(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            paths, _, new = self._installed_cache_pair(Path(temporary))
            new_bytes = cli._support_update_cache_bytes(new)
            real_exchange = cli._exchange_paths
            real_fsync = cli._fsync_directory
            final_exchange = False
            post_exchange_fsyncs = 0
            injected = False
            output: list[str] = []

            def record_final_exchange(first: Path, second: Path) -> None:
                nonlocal final_exchange
                real_exchange(first, second)
                if (second == paths.support_update_cache
                        and (first.name == "commit" or first.name.endswith("-commit"))):
                    final_exchange = True

            def fail_cleanup_fsync(path: Path) -> None:
                nonlocal post_exchange_fsyncs, injected
                if final_exchange:
                    post_exchange_fsyncs += 1
                    if post_exchange_fsyncs == 2:
                        injected = True
                        raise cli.DesktopCLIError(
                            "injected post-commit cleanup durability failure"
                        )
                real_fsync(path)

            with mock.patch.object(
                    cli, "_exchange_paths", side_effect=record_final_exchange,
            ), mock.patch.object(
                    cli, "_fsync_directory", side_effect=fail_cleanup_fsync,
            ):
                cli._record_support_update_cache(paths, new, output.append)
            self.assertTrue(final_exchange)
            self.assertTrue(injected)
            self.assertGreaterEqual(post_exchange_fsyncs, 2)
            self.assertEqual(len(output), 1)
            self.assertRegex(output[0], "committed.*cleanup|cleanup.*committed")
            self.assertLessEqual(len(output[0]), 600)
            self.assertNotIn("Traceback", output[0])
            canonical_identity = cli._identity(paths.support_update_cache)
            self._assert_exact_cache(
                paths.support_update_cache, new_bytes, canonical_identity,
            )
            self.assertEqual(cli._read_support_update_cache(paths), new)
            self.assertEqual(self._cache_transaction_entries(paths), [])

    def test_support_cache_primary_failure_is_not_masked_by_cleanup_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            paths, old, new = self._installed_cache_pair(Path(temporary))
            prior_bytes = paths.support_update_cache.read_bytes()
            prior_identity = cli._identity(paths.support_update_cache)
            real_fsync = cli._fsync_directory
            real_unlink = Path.unlink
            primary_injected = False
            cleanup_injected = False

            def fail_primary_once(path: Path) -> None:
                nonlocal primary_injected
                if path == paths.support and not primary_injected:
                    primary_injected = True
                    raise cli.DesktopCLIError(
                        "injected primary support cache transaction failure"
                    )
                real_fsync(path)

            def fail_one_cleanup_unlink(path: Path, *args, **kwargs) -> None:
                nonlocal cleanup_injected
                if (not cleanup_injected
                        and (path.name == "commit" or path.name.endswith("-commit"))):
                    cleanup_injected = True
                    raise PermissionError("injected secondary cleanup refusal")
                real_unlink(path, *args, **kwargs)

            caught: BaseException | None = None
            with mock.patch.object(
                    cli, "_fsync_directory", side_effect=fail_primary_once,
            ), mock.patch.object(Path, "unlink", new=fail_one_cleanup_unlink):
                try:
                    cli._write_support_update_cache(paths, new)
                except BaseException as error:
                    caught = error
            self.assertTrue(primary_injected)
            self.assertTrue(cleanup_injected)
            self.assertIsInstance(caught, cli.DesktopCLIError)
            assert caught is not None
            self.assertIn("injected primary support cache transaction failure", str(caught))
            self.assertIn("cleanup", str(caught).lower())
            self.assertLessEqual(len(str(caught)), 600)
            self.assertNotIn("Traceback", str(caught))
            self._assert_exact_cache(
                paths.support_update_cache, prior_bytes, prior_identity,
            )
            self.assertEqual(cli._read_support_update_cache(paths), old)
            self.assertEqual(self._cache_transaction_entries(paths), [])

    def test_support_cache_contract_names_cooperative_private_and_canonical_boundaries(self) -> None:
        repository = Path(__file__).resolve().parents[1]
        documents = "\n".join(
            (repository / relative).read_text()
            for relative in (
                "ANATOMY.md", "CONTRACT.md", "tests/ANATOMY.md", "tests/CONTRACT.md",
            )
        )
        self.assertIn("cooperative private transaction namespace", documents)
        self.assertIn(
            "arbitrary uncooperative same-UID replacement inside that namespace is outside",
            documents,
        )
        self.assertIn("canonical arbitrary-racer boundary", documents)
        self.assertIn(
            "preserved canonical-racer diagnostics are not ordinary updater-owned residue",
            documents,
        )
        self.assertNotIn("later cleanup removes only recorded identities", documents)

    def test_official_stage_bootstrap_switch_commits_once_and_preserves_app_identity(self) -> None:
        import importlib
        bootstrap = importlib.import_module("scripts.support_bootstrap")
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
            original_cwd = Path.cwd()
            staged = cli.stage_official_support_update(
                home=home, transport=transport, effective_uid=501,
                argv=[str(paths.launcher), "doctor"], environment={"HOME": str(home)},
                exec_launcher=lambda path, argv, env: executions.append((path, argv, env)),
            )
            self.assertIsNotNone(staged)
            assert staged is not None
            self.assertEqual(Path.cwd(), original_cwd)
            delegated: list[tuple[Path, list[str]]] = []
            with mock.patch.dict(os.environ, executions[0][2], clear=True):
                self.assertEqual(bootstrap.run_launcher(
                    ["doctor"], home=home, self_test_runner=lambda _: None,
                    installed_runner=lambda path, argv: delegated.append((path, list(argv))) or 23,
                ), 23)
            self.assertEqual(os.readlink(paths.support_current), f"versions/{staged}")
            state, _ = cli._read_support_state(paths)
            self.assertEqual(state.last_good_generation, staged)
            self.assertIsNone(cli._read_support_pending(paths))
            self.assertEqual(delegated[0][0].parent.name, staged)
            self.assertEqual(delegated[0][1], ["doctor"])
            self.assertEqual(tree_snapshot(paths.versions), app_before)

    def test_bootstrap_consumes_marker_before_import_and_exposes_one_shot_guard(self) -> None:
        source = (Path(__file__).resolve().parents[1] / "scripts/support_bootstrap.py").read_text()
        self.assertIn("_SUPPORT_REEXEC_CONSUMED", source)
        self.assertIn("os.environ.pop(SUPPORT_REEXEC_MARKER", source)
        self.assertNotIn("os.environ.get(SUPPORT_REEXEC_MARKER", source)


if __name__ == "__main__":
    unittest.main()
