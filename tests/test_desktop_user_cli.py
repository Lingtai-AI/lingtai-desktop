#!/usr/bin/env python3
"""Hermetic contracts for the user-level LingTai Desktop installer/launcher."""

from __future__ import annotations

import hashlib
import json
import os
import plistlib
import stat
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path
from unittest import mock

from scripts import desktop_user_cli as cli


def write_artifacts(root: Path, version: str = "0.1.5", *, release: bool = False) -> tuple[Path, Path]:
    dmg = root / f"LingTai-{version}-macOS-universal.dmg"
    dmg.write_bytes(b"fixed test dmg")
    manifest = root / f"LingTai-{version}-macOS-universal.manifest.json"
    manifest.write_text(json.dumps({
        "architectures": ["arm64", "x86_64"],
        "file_name": dmg.name,
        "minimum_macos": "13.0",
        "notarization": ("Apple accepted; ticket stapled and validated" if release else "not performed (diagnostic mode)"),
        "packaging_git_dirty": False,
        "packaging_git_sha": "a" * 40,
        "packaging_git_tree": "b" * 40,
        "sha256": hashlib.sha256(dmg.read_bytes()).hexdigest(),
        "signing": ("Developer ID Application; hardened runtime; timestamped" if release else "ad-hoc App; unsigned DMG (diagnostic only)"),
        "size_bytes": dmg.stat().st_size,
        "version": version,
    }, sort_keys=True), encoding="utf-8")
    return dmg, manifest


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
        self.current_during_copy: str | None = None

    def verify_package(self, repository_root: Path, dmg: Path, version: str, release_ready: bool) -> None:
        self.calls.append(["verify", str(dmg), version, str(release_ready)])
        if self.fail == "verifier":
            raise cli.DesktopCLIError("injected verifier failure")

    @contextmanager
    def mounted_app(self, dmg: Path, scratch: Path):
        if self.fail == "mount":
            raise cli.DesktopCLIError("injected mount failure")
        source = scratch / "mounted" / "LingTai.app"
        make_app(source, cli.VERSION_PATTERN.search(dmg.name).group(0))
        yield source

    def copy_app(self, source: Path, destination: Path) -> None:
        if self.fail == "copy":
            raise cli.DesktopCLIError("injected copy failure")
        import shutil
        shutil.copytree(source, destination, symlinks=True)

    def smoke(self, executable: Path, fake_home: Path, fake_tmp: Path) -> None:
        self.calls.append([str(executable), "--smoke", str(fake_home), str(fake_tmp)])

    def open_app(self, app: Path) -> None:
        self.calls.append(["/usr/bin/open", str(app)])

    def exec_app(self, executable: Path, arguments: list[str]) -> None:
        self.exec_calls.append([str(executable), *arguments])


class DesktopUserCLIContractTest(unittest.TestCase):
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
            dmg, manifest = write_artifacts(root)
            cli.install(dmg, manifest, home=home, allow_diagnostic=True,
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
            dmg, manifest = write_artifacts(root)
            verifier = home / ".local/share/lingtai-desktop/cli/verify-macos-package.py"
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
                    cli.install(dmg, manifest, home=home, allow_diagnostic=True,
                                platform=FakePlatform(), effective_uid=501)
            self.assertIsNotNone(racer_bytes)
            self.assertEqual(verifier.read_bytes(), racer_bytes)

    def test_uninstall_refuses_symlinked_managed_root_without_touching_outside_or_launcher(self) -> None:
        for command in ("version", "all"):
            with self.subTest(command=command), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                home = root / "home"
                home.mkdir()
                dmg, manifest = write_artifacts(root)
                cli.install(dmg, manifest, home=home, allow_diagnostic=True,
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
                dmg1, manifest1 = write_artifacts(root, "0.1.5")
                cli.install(dmg1, manifest1, home=home, allow_diagnostic=True,
                            platform=platform, effective_uid=501)
                if corruption == "unknown-root":
                    (home / ".local/share/lingtai-desktop/KEEP").write_text("unknown")
                else:
                    newer = root / "newer"
                    newer.mkdir()
                    dmg2, manifest2 = write_artifacts(newer, "0.1.6")
                    cli.install(dmg2, manifest2, home=home, allow_diagnostic=True,
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

    def test_bootstrap_warning_uses_validated_classification_not_opt_in_flag(self) -> None:
        release_receipt = {"classification": "release-ready"}
        with mock.patch.object(cli, "install", return_value="0.1.5"), \
             mock.patch.object(cli, "doctor", return_value=("0.1.5", release_receipt)), \
             mock.patch("builtins.print") as output:
            self.assertEqual(cli.bootstrap_main([
                "--dmg", "/tmp/release.dmg", "--manifest", "/tmp/release.json",
                "--allow-diagnostic",
            ]), 0)
        self.assertFalse(any("WARNING" in str(call) for call in output.call_args_list))

    def test_manifest_exact_schema_hash_size_state_and_symlink_refusal(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            dmg, manifest = write_artifacts(root)
            parsed = cli.load_manifest(dmg, manifest, allow_diagnostic=True)
            self.assertEqual(parsed.version, "0.1.5")
            self.assertTrue(parsed.diagnostic)
            with self.assertRaisesRegex(cli.DesktopCLIError, "explicit --allow-diagnostic"):
                cli.load_manifest(dmg, manifest, allow_diagnostic=False)

            data = json.loads(manifest.read_text())
            data["unknown"] = True
            manifest.write_text(json.dumps(data))
            with self.assertRaisesRegex(cli.DesktopCLIError, "exact schema"):
                cli.load_manifest(dmg, manifest, allow_diagnostic=True)
            data.pop("unknown")
            data["sha256"] = "0" * 64
            manifest.write_text(json.dumps(data))
            with self.assertRaisesRegex(cli.DesktopCLIError, "SHA-256"):
                cli.load_manifest(dmg, manifest, allow_diagnostic=True)
            manifest.unlink()
            manifest.symlink_to(root / "elsewhere")
            with self.assertRaisesRegex(cli.DesktopCLIError, "regular file, not a symlink"):
                cli.load_manifest(dmg, manifest, allow_diagnostic=True)

    def test_release_uses_strict_verifier_and_install_layout_is_private(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            dmg, manifest = write_artifacts(root, release=True)
            platform = FakePlatform()
            cli.install(dmg, manifest, home=home, allow_diagnostic=False, platform=platform, effective_uid=501)
            managed = home / ".local/share/lingtai-desktop"
            self.assertEqual(os.readlink(managed / "current"), "versions/0.1.5")
            self.assertTrue((managed / "versions/0.1.5/LingTai.app").is_dir())
            self.assertTrue((managed / "receipts/0.1.5.json").is_file())
            self.assertTrue((managed / "cli/desktop_user_cli.py").is_file())
            launcher = home / ".local/bin/lingtai-desktop"
            self.assertEqual(stat.S_IMODE(launcher.stat().st_mode), 0o755)
            self.assertEqual(platform.calls[0][-1], "True")

    def test_same_version_update_is_verified_byte_identical_idempotence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            dmg, manifest = write_artifacts(root)
            platform = FakePlatform()
            cli.install(dmg, manifest, home=home, allow_diagnostic=True, platform=platform, effective_uid=501)
            managed = home / ".local/share/lingtai-desktop"
            before = (os.readlink(managed / "current"), (managed / "receipts/0.1.5.json").read_bytes())
            cli.install(dmg, manifest, home=home, allow_diagnostic=True, platform=platform, effective_uid=501, update=True)
            self.assertEqual((os.readlink(managed / "current"), (managed / "receipts/0.1.5.json").read_bytes()), before)
            self.assertEqual([call[0] for call in platform.calls].count("verify"), 2)

    def test_launch_version_doctor_and_tamper_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            dmg, manifest = write_artifacts(root)
            platform = FakePlatform()
            cli.install(dmg, manifest, home=home, allow_diagnostic=True, platform=platform, effective_uid=501)
            out: list[str] = []
            cli.run_installed([], home=home, platform=platform, output=out.append)
            app = home / ".local/share/lingtai-desktop/versions/0.1.5/LingTai.app"
            self.assertEqual(platform.calls[-1], ["/usr/bin/open", str(app)])
            cli.run_installed(["foreground", "--", "--smoke"], home=home, platform=platform, output=out.append)
            self.assertEqual(platform.exec_calls[-1], [str(app / "Contents/MacOS/LingTai"), "--smoke"])
            cli.run_installed(["version"], home=home, platform=platform, output=out.append)
            cli.run_installed(["doctor"], home=home, platform=platform, output=out.append)
            self.assertIn("INTEGRITY PASS", "\n".join(out))
            self.assertIn("NOT RELEASE READY", "\n".join(out))
            (app / "Contents/MacOS/LingTai").write_bytes(b"tampered")
            with self.assertRaisesRegex(cli.DesktopCLIError, "bundle digest"):
                cli.run_installed(["open"], home=home, platform=platform, output=out.append)

            make_app(app, "0.1.5")
            verifier = home / ".local/share/lingtai-desktop/cli/verify-macos-package.py"
            verifier.write_text("tampered verifier")
            with self.assertRaisesRegex(cli.DesktopCLIError, "unrelated launcher"):
                cli.run_installed(["doctor"], home=home, platform=platform, output=out.append)

    def test_update_failure_matrix_preserves_old_current_and_owned_bytes(self) -> None:
        for failure in ("verifier", "mount", "copy", "receipt", "launcher", "current"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                home = root / "home"
                home.mkdir()
                platform = FakePlatform()
                dmg1, manifest1 = write_artifacts(root, "0.1.5")
                cli.install(dmg1, manifest1, home=home, allow_diagnostic=True, platform=platform, effective_uid=501)
                managed = home / ".local/share/lingtai-desktop"
                launcher = home / ".local/bin/lingtai-desktop"
                old = (os.readlink(managed / "current"), launcher.read_bytes(), (managed / "receipts/0.1.5.json").read_bytes())
                newer = root / "new"
                newer.mkdir()
                dmg2, manifest2 = write_artifacts(newer, "0.1.6")
                platform.fail = failure if failure in {"verifier", "mount", "copy"} else None
                with mock.patch.object(cli, "_FAILPOINT", failure), self.assertRaises(cli.DesktopCLIError):
                    cli.install(dmg2, manifest2, home=home, allow_diagnostic=True, platform=platform, effective_uid=501, update=True)
                self.assertEqual((os.readlink(managed / "current"), launcher.read_bytes(), (managed / "receipts/0.1.5.json").read_bytes()), old)
                self.assertFalse((managed / "versions/0.1.6").exists())
                self.assertFalse((managed / "receipts/0.1.6.json").exists())

    def test_collision_symlink_root_traversal_uninstall_and_root_refusal(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            dmg, manifest = write_artifacts(root)
            launcher = home / ".local/bin/lingtai-desktop"
            launcher.parent.mkdir(parents=True)
            launcher.write_text("unrelated")
            with self.assertRaisesRegex(cli.DesktopCLIError, "unrelated launcher"):
                cli.install(dmg, manifest, home=home, allow_diagnostic=True, platform=FakePlatform(), effective_uid=501)
            launcher.unlink()
            with self.assertRaisesRegex(cli.DesktopCLIError, "effective uid 0"):
                cli.install(dmg, manifest, home=home, allow_diagnostic=True, platform=FakePlatform(), effective_uid=0)
            managed = home / ".local/share/lingtai-desktop"
            import shutil
            shutil.rmtree(managed)
            managed.symlink_to(root / "outside")
            with self.assertRaisesRegex(cli.DesktopCLIError, "symlink"):
                cli.install(dmg, manifest, home=home, allow_diagnostic=True, platform=FakePlatform(), effective_uid=501)

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            home = root / "home"
            home.mkdir()
            dmg, manifest = write_artifacts(root)
            platform = FakePlatform()
            cli.install(dmg, manifest, home=home, allow_diagnostic=True, platform=platform, effective_uid=501)
            with self.assertRaisesRegex(cli.DesktopCLIError, "safe x.y.z"):
                cli.uninstall_version("../../outside", home=home, effective_uid=501)
            cli.uninstall_version("0.1.5", home=home, effective_uid=501)
            self.assertFalse((home / ".local/share/lingtai-desktop/versions/0.1.5").exists())
            dmg, manifest = write_artifacts(root, "0.1.6")
            cli.install(dmg, manifest, home=home, allow_diagnostic=True, platform=platform, effective_uid=501)
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

            dmg, manifest = write_artifacts(root)
            platform = FakePlatform()
            cli.install(dmg, manifest, home=home, allow_diagnostic=True, platform=platform, effective_uid=501)
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
            dmg, manifest = write_artifacts(root)
            collision = home / ".local/share/lingtai-desktop/versions/0.1.5"
            collision.mkdir(parents=True)
            with self.assertRaisesRegex(cli.DesktopCLIError, "version collision"):
                cli.install(dmg, manifest, home=home, allow_diagnostic=True,
                            platform=FakePlatform(), effective_uid=501)


if __name__ == "__main__":
    unittest.main()
