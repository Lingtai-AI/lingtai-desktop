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
            cli._write_support_update_cache(paths, check)
            self.assertEqual(cli._read_support_update_cache(paths), check)
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
                 if path.name.startswith(".update-check-")},
                set(),
            )

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
