#!/usr/bin/env python3
"""Hermetic contracts for portable LingTai.app archive production/verification."""

from __future__ import annotations

import importlib.util
import hashlib
import json
import os
import plistlib
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from scripts import app_archive, desktop_user_cli


_VERIFIER_SPEC = importlib.util.spec_from_file_location(
    "verify_app_archive",
    Path(__file__).parents[1] / "scripts" / "verify-app-archive.py",
)
assert _VERIFIER_SPEC is not None and _VERIFIER_SPEC.loader is not None
verify_app_archive = importlib.util.module_from_spec(_VERIFIER_SPEC)
_VERIFIER_SPEC.loader.exec_module(verify_app_archive)


def make_app(path: Path, version: str = "0.1.5") -> None:
    executable = path / "Contents/MacOS/LingTai"
    executable.parent.mkdir(parents=True)
    executable.write_bytes(b"#!/bin/sh\nexit 0\n")
    executable.chmod(0o755)
    framework = path / "Contents/Frameworks/Example.framework/Versions/A"
    framework.mkdir(parents=True)
    os.symlink("A", framework.parent / "Current")
    os.symlink("Versions/Current", framework.parent.parent / "Example")
    resource = path / "Contents/Resources/payload.bin"
    resource.parent.mkdir(parents=True)
    resource.write_bytes(b"hard-linked payload")
    os.link(resource, resource.with_name("payload-copy.bin"))
    with (path / "Contents/Info.plist").open("wb") as stream:
        plistlib.dump({
            "CFBundleIdentifier": "ai.lingtai.desktop",
            "CFBundleShortVersionString": version,
            "CFBundleVersion": version,
            "CFBundleExecutable": "LingTai",
            "LSMinimumSystemVersion": "13.0",
        }, stream)


class AppArchiveContractTest(unittest.TestCase):
    @staticmethod
    def _architectures(_: Path) -> tuple[str, ...]:
        return "arm64", "x86_64"

    def _package(self, root: Path, version: str = "0.1.5") -> tuple[Path, Path, Path]:
        app = root / "input/LingTai.app"
        make_app(app, version)
        output = root / "output"
        output.mkdir()
        with mock.patch.object(
            app_archive,
            "packaging_git_facts",
            return_value=app_archive.PackagingGitFacts("a" * 40, "b" * 40, False),
        ):
            archive, manifest = app_archive.package_app_archive(
                app,
                output,
                repository_root=Path(__file__).parents[1],
                architecture_reader=self._architectures,
                verifier_runner=lambda archive_path, manifest_path: verify_app_archive.verify_pair(
                    archive_path,
                    manifest_path,
                    architecture_reader=self._architectures,
                ),
            )
        return archive, manifest, app

    @staticmethod
    def _rewrite_archive_binding(archive: Path, manifest: Path) -> None:
        data = json.loads(manifest.read_text())
        data["archive_size_bytes"] = archive.stat().st_size
        data["archive_sha256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
        manifest.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")

    @staticmethod
    def _write_members(archive: Path, members: list[tarfile.TarInfo]) -> None:
        with tarfile.open(archive, "w:gz", format=tarfile.PAX_FORMAT) as output:
            for member in members:
                if member.isfile():
                    import io
                    output.addfile(member, io.BytesIO(b"x" * member.size))
                else:
                    output.addfile(member)

    def test_packaged_pair_is_independently_verified_and_preserves_modes_and_links(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest, _ = self._package(root)

            extracted = root / "verified"
            verify_app_archive.verify_pair(
                archive,
                manifest,
                extract_to=extracted,
                architecture_reader=self._architectures,
            )
            verified_app = extracted / "LingTai.app"
            self.assertEqual(
                (verified_app / "Contents/MacOS/LingTai").stat().st_mode & 0o777,
                0o755,
            )
            self.assertEqual(
                os.readlink(verified_app / "Contents/Frameworks/Example.framework/Versions/Current"),
                "A",
            )
            first = verified_app / "Contents/Resources/payload.bin"
            second = verified_app / "Contents/Resources/payload-copy.bin"
            self.assertEqual((first.stat().st_dev, first.stat().st_ino),
                             (second.stat().st_dev, second.stat().st_ino))

    def test_symlink_mode_restoration_is_umask_independent_and_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            previous_umask = os.umask(0o022)
            try:
                archive, manifest, source_app = self._package(root)
            finally:
                os.umask(previous_umask)

            relative_link = Path("Contents/Frameworks/Example.framework/Versions/Current")
            expected_mode = os.lstat(source_app / relative_link).st_mode & 0o777
            self.assertEqual(expected_mode, 0o755)
            expected_digest = json.loads(manifest.read_text())["bundle_tree_sha256"]
            extracted_digests: list[str] = []

            for label, extraction_umask in (("ordinary", 0o022), ("restrictive", 0o077)):
                with self.subTest(label=label):
                    destination = root / f"verified-{label}"
                    previous_umask = os.umask(extraction_umask)
                    try:
                        verify_app_archive.verify_pair(
                            archive,
                            manifest,
                            extract_to=destination,
                            architecture_reader=self._architectures,
                        )
                    finally:
                        os.umask(previous_umask)
                    extracted_app = destination / "LingTai.app"
                    extracted_link = extracted_app / relative_link
                    self.assertEqual(os.readlink(extracted_link), "A")
                    self.assertEqual(os.lstat(extracted_link).st_mode & 0o777, expected_mode)
                    digest = verify_app_archive._bundle_tree_digest(extracted_app)
                    self.assertEqual(digest, expected_digest)
                    extracted_digests.append(digest)

            self.assertEqual(extracted_digests, [expected_digest, expected_digest])

            real_chmod = verify_app_archive.os.chmod

            def deny_descriptor_local_symlink_mode(
                path: os.PathLike[str] | str,
                mode: int,
                *,
                dir_fd: int | None = None,
                follow_symlinks: bool = True,
            ) -> None:
                if dir_fd is not None and not follow_symlinks:
                    raise OSError("injected symlink mode failure")
                real_chmod(path, mode, dir_fd=dir_fd, follow_symlinks=follow_symlinks)

            failed_destination = root / "must-not-survive"
            with mock.patch.object(
                verify_app_archive.os,
                "chmod",
                side_effect=deny_descriptor_local_symlink_mode,
            ), self.assertRaisesRegex(
                verify_app_archive.VerificationError, "symlink mode"
            ):
                verify_app_archive.verify_pair(
                    archive,
                    manifest,
                    extract_to=failed_destination,
                    architecture_reader=self._architectures,
                )
            self.assertFalse(failed_destination.exists())

    def test_manifest_and_exact_app_fact_mismatches_are_rejected(self) -> None:
        mutations = {
            "archive hash": lambda data: data.__setitem__("archive_sha256", "0" * 64),
            "archive size": lambda data: data.__setitem__("archive_size_bytes", data["archive_size_bytes"] + 1),
            "bundle": lambda data: data.__setitem__("bundle_identifier", "invalid.bundle"),
            "executable hash": lambda data: data.__setitem__("executable_sha256", "0" * 64),
            "executable size": lambda data: data.__setitem__("executable_size_bytes", data["executable_size_bytes"] + 1),
            "architectures": lambda data: data.__setitem__("architectures", ["arm64"]),
            "recursive digest": lambda data: data.__setitem__("bundle_tree_sha256", "0" * 64),
        }
        for label, mutate in mutations.items():
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                archive, manifest, _ = self._package(Path(temporary))
                data = json.loads(manifest.read_text())
                mutate(data)
                manifest.write_text(json.dumps(data))
                with self.assertRaises(verify_app_archive.VerificationError):
                    verify_app_archive.verify_pair(
                        archive, manifest, architecture_reader=self._architectures
                    )

        with tempfile.TemporaryDirectory() as temporary:
            archive, manifest, _ = self._package(Path(temporary))
            with self.assertRaisesRegex(verify_app_archive.VerificationError, "architectures"):
                verify_app_archive.verify_pair(
                    archive,
                    manifest,
                    architecture_reader=lambda _: ("arm64",),
                )

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest, _ = self._package(root)
            renamed = archive.with_name("LingTai-0.1.6-macOS-universal.app.tar.gz")
            archive.rename(renamed)
            data = json.loads(manifest.read_text())
            data["version"] = "0.1.6"
            data["bundle_version"] = "0.1.6"
            data["archive_file_name"] = renamed.name
            manifest.write_text(json.dumps(data))
            with self.assertRaisesRegex(verify_app_archive.VerificationError, "bundle identity"):
                verify_app_archive.verify_pair(
                    renamed, manifest, architecture_reader=self._architectures
                )

    def test_untrusted_archive_structure_is_rejected_before_extraction(self) -> None:
        root_directory = tarfile.TarInfo("LingTai.app")
        root_directory.type = tarfile.DIRTYPE
        root_directory.mode = 0o755

        cases: dict[str, list[tarfile.TarInfo]] = {}
        absolute = tarfile.TarInfo("/LingTai.app/escape")
        absolute.size = 1
        cases["absolute"] = [root_directory, absolute]
        traversal = tarfile.TarInfo("LingTai.app/../escape")
        traversal.size = 1
        cases["traversal"] = [root_directory, traversal]
        invalid = tarfile.TarInfo("LingTai.app/bad\\name")
        invalid.size = 1
        cases["invalid"] = [root_directory, invalid]
        escaping_symlink = tarfile.TarInfo("LingTai.app/bad-link")
        escaping_symlink.type = tarfile.SYMTYPE
        escaping_symlink.linkname = "../../escape"
        cases["escaping symlink"] = [root_directory, escaping_symlink]
        escaping_hardlink = tarfile.TarInfo("LingTai.app/bad-hardlink")
        escaping_hardlink.type = tarfile.LNKTYPE
        escaping_hardlink.linkname = "../escape"
        cases["escaping hardlink"] = [root_directory, escaping_hardlink]
        fifo = tarfile.TarInfo("LingTai.app/fifo")
        fifo.type = tarfile.FIFOTYPE
        cases["FIFO"] = [root_directory, fifo]
        device = tarfile.TarInfo("LingTai.app/device")
        device.type = tarfile.CHRTYPE
        cases["device"] = [root_directory, device]
        socket = tarfile.TarInfo("LingTai.app/socket")
        socket.type = b"s"
        cases["socket"] = [root_directory, socket]
        extra = tarfile.TarInfo("README")
        extra.size = 1
        cases["extra top-level"] = [root_directory, extra]
        duplicate = tarfile.TarInfo("LingTai.app")
        duplicate.type = tarfile.DIRTYPE
        cases["duplicate"] = [root_directory, duplicate]
        conflicting = tarfile.TarInfo("LingTai.app")
        conflicting.size = 1
        cases["conflicting"] = [root_directory, conflicting]

        for label, members in cases.items():
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                archive, manifest, _ = self._package(root)
                self._write_members(archive, members)
                self._rewrite_archive_binding(archive, manifest)
                destination = root / "must-not-survive"
                with self.assertRaises(verify_app_archive.VerificationError):
                    verify_app_archive.verify_pair(
                        archive,
                        manifest,
                        extract_to=destination,
                        architecture_reader=self._architectures,
                    )
                self.assertFalse(destination.exists())

    def test_link_payload_is_rejected_without_advancing_member_scan(self) -> None:
        for link_type in (tarfile.SYMTYPE, tarfile.LNKTYPE):
            with self.subTest(link_type=link_type):
                link = tarfile.TarInfo("LingTai.app/bad-link")
                link.type = link_type
                link.linkname = "LingTai.app/target" if link_type == tarfile.LNKTYPE else "target"
                link.size = verify_app_archive.MAX_TOTAL_BYTES + 1

                class SentinelMembers:
                    def __init__(self) -> None:
                        self.calls = 0

                    def __iter__(self) -> SentinelMembers:
                        return self

                    def __next__(self) -> tarfile.TarInfo:
                        self.calls += 1
                        if self.calls == 1:
                            return link
                        raise AssertionError("preflight advanced beyond the invalid link header")

                members = SentinelMembers()
                with self.assertRaisesRegex(
                    verify_app_archive.VerificationError, "link has conflicting data"
                ):
                    verify_app_archive._preflight_members(members)
                self.assertEqual(members.calls, 1)

    def test_directory_without_owner_access_is_rejected_before_extraction(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, manifest, _ = self._package(root)
            app_root = tarfile.TarInfo("LingTai.app")
            app_root.type = tarfile.DIRTYPE
            app_root.mode = 0o755
            locked = tarfile.TarInfo("LingTai.app/locked")
            locked.type = tarfile.DIRTYPE
            locked.mode = 0o000
            self._write_members(archive, [app_root, locked])
            self._rewrite_archive_binding(archive, manifest)
            destination = root / "must-not-survive"
            with self.assertRaisesRegex(
                verify_app_archive.VerificationError, "required owner permissions"
            ):
                verify_app_archive.verify_pair(
                    archive,
                    manifest,
                    extract_to=destination,
                    architecture_reader=self._architectures,
                )
            self.assertFalse(destination.exists())

    def test_recursive_bundle_digests_stream_regular_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary) / "LingTai.app"
            make_app(app)
            expected = app_archive._bundle_tree_digest(app)
            self.assertEqual(verify_app_archive._bundle_tree_digest(app), expected)
            self.assertEqual(desktop_user_cli.bundle_tree_digest(app), expected)
            with mock.patch.object(
                Path, "read_bytes", side_effect=AssertionError("whole-file read forbidden")
            ):
                self.assertEqual(app_archive._bundle_tree_digest(app), expected)
                self.assertEqual(verify_app_archive._bundle_tree_digest(app), expected)
                self.assertEqual(desktop_user_cli.bundle_tree_digest(app), expected)

    def test_oversized_sparse_archive_is_rejected_before_hashing(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "LingTai-0.1.5-macOS-universal.app.tar.gz"
            with archive.open("wb") as stream:
                stream.truncate(verify_app_archive.MAX_ARCHIVE_BYTES + 1)
            manifest = root / "manifest.json"
            manifest.write_text("{}")
            for module, verify, error_type in (
                (verify_app_archive, verify_app_archive.verify_pair,
                 verify_app_archive.VerificationError),
                (desktop_user_cli, desktop_user_cli.load_manifest,
                 desktop_user_cli.DesktopCLIError),
            ):
                with self.subTest(module=module.__name__), \
                     mock.patch.object(
                         module.hashlib, "sha256",
                         side_effect=AssertionError("oversized archive was hashed"),
                     ), self.assertRaisesRegex(error_type, "archive is too large"):
                    verify(archive, manifest)

    def test_nonregular_open_descriptor_is_rejected_for_archive_and_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "LingTai-0.1.5-macOS-universal.app.tar.gz"
            archive.write_bytes(b"archive")
            manifest = root / "manifest.json"
            manifest.write_text("{}")
            original_open = os.open
            read_descriptor, write_descriptor = os.pipe()
            os.set_blocking(read_descriptor, False)
            try:
                for module, loader, error_type in (
                    (verify_app_archive, verify_app_archive.load_manifest,
                     verify_app_archive.VerificationError),
                    (desktop_user_cli, desktop_user_cli.load_manifest,
                     desktop_user_cli.DesktopCLIError),
                ):
                    for target, label in ((archive, "App archive"), (manifest, "manifest")):
                        with self.subTest(module=module.__name__, label=label):
                            def open_with_fifo(path: os.PathLike[str] | str, flags: int,
                                               *args: object, **kwargs: object) -> int:
                                if Path(path) == target:
                                    self.assertTrue(flags & getattr(os, "O_NONBLOCK", 0))
                                    return os.dup(read_descriptor)
                                return original_open(path, flags, *args, **kwargs)

                            with mock.patch.object(module.os, "open", side_effect=open_with_fifo), \
                                 self.assertRaisesRegex(error_type, "regular file"):
                                loader(archive, manifest)
            finally:
                os.close(read_descriptor)
                os.close(write_descriptor)

    def test_malformed_and_truncated_archives_are_rejected_and_cleaned(self) -> None:
        for form in ("malformed", "truncated"):
            with self.subTest(form=form), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                archive, manifest, _ = self._package(root)
                if form == "malformed":
                    archive.write_bytes(b"not a gzip or tar")
                else:
                    content = archive.read_bytes()
                    archive.write_bytes(content[: len(content) // 2])
                self._rewrite_archive_binding(archive, manifest)
                destination = root / "extracted"
                with self.assertRaisesRegex(verify_app_archive.VerificationError, "malformed|truncated"):
                    verify_app_archive.verify_pair(
                        archive,
                        manifest,
                        extract_to=destination,
                        architecture_reader=self._architectures,
                    )
                self.assertFalse(destination.exists())

    def test_pair_publication_is_exclusive_and_rollback_is_inode_bound(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            staged_archive = root / "staged.tar.gz"
            staged_manifest = root / "staged.json"
            final_archive = root / "final.tar.gz"
            final_manifest = root / "final.json"
            staged_archive.write_bytes(b"archive")
            staged_manifest.write_bytes(b"manifest")
            final_manifest.write_bytes(b"racer")
            with self.assertRaisesRegex(app_archive.ArchivePackagingError, "manifest appeared"):
                app_archive.publish_pair(
                    staged_archive, staged_manifest, final_archive, final_manifest
                )
            self.assertFalse(final_archive.exists())
            self.assertEqual(final_manifest.read_bytes(), b"racer")

            real_link = os.link

            def replace_before_second(source: Path, destination: Path, **kwargs: object) -> None:
                if Path(destination) == final_manifest:
                    final_archive.unlink()
                    final_archive.write_bytes(b"foreign replacement")
                real_link(source, destination, **kwargs)

            with mock.patch.object(app_archive.os, "link", side_effect=replace_before_second), \
                 self.assertRaisesRegex(app_archive.ArchivePackagingError, "manifest appeared"):
                app_archive.publish_pair(
                    staged_archive, staged_manifest, final_archive, final_manifest
                )
            self.assertEqual(final_archive.read_bytes(), b"foreign replacement")
            self.assertEqual(final_manifest.read_bytes(), b"racer")

    def test_pair_publication_rejects_successful_second_link_archive_race(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            staged_archive = root / "staged.tar.gz"
            staged_manifest = root / "staged.json"
            final_archive = root / "final.tar.gz"
            final_manifest = root / "final.json"
            staged_archive.write_bytes(b"archive")
            staged_manifest.write_bytes(b"manifest")
            real_link = os.link

            def replace_archive_then_publish_manifest(
                source: Path, destination: Path, **kwargs: object
            ) -> None:
                if Path(destination) == final_manifest:
                    final_archive.unlink()
                    final_archive.write_bytes(b"foreign replacement")
                real_link(source, destination, **kwargs)

            with mock.patch.object(
                app_archive.os, "link", side_effect=replace_archive_then_publish_manifest
            ), self.assertRaisesRegex(
                app_archive.ArchivePackagingError, "pair changed during publication"
            ):
                app_archive.publish_pair(
                    staged_archive, staged_manifest, final_archive, final_manifest
                )
            self.assertEqual(final_archive.read_bytes(), b"foreign replacement")
            self.assertFalse(final_manifest.exists())


if __name__ == "__main__":
    unittest.main()
