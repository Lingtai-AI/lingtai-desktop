#!/usr/bin/env python3
"""Deterministic, offline contracts for the macOS packaging boundary."""

from __future__ import annotations

import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from scripts import macos_packaging


_VERIFIER_SPEC = importlib.util.spec_from_file_location(
    "verify_macos_package",
    Path(__file__).parents[1] / "scripts" / "verify-macos-package.py",
)
assert _VERIFIER_SPEC is not None and _VERIFIER_SPEC.loader is not None
verify_macos_package = importlib.util.module_from_spec(_VERIFIER_SPEC)
_VERIFIER_SPEC.loader.exec_module(verify_macos_package)


class MacOSPackagingContractTest(unittest.TestCase):
    def test_modes_fail_closed_and_never_expose_credential_values(self) -> None:
        secret_identity = "Developer ID Application: Private Example (ABCDE12345)"
        secret_profile = "private-notary-profile"

        with self.assertRaisesRegex(
            macos_packaging.PackagingError,
            "release mode requires --signing-identity or LINGTAI_CODESIGN_IDENTITY",
        ):
            macos_packaging.resolve_release_credentials("release", None, None, {})

        invalid_identity = "Private Example Identity (ABCDE12345)"
        with self.assertRaises(macos_packaging.PackagingError) as caught:
            macos_packaging.resolve_release_credentials(
                "release", invalid_identity, None, {"LINGTAI_NOTARY_PROFILE": secret_profile}
            )
        self.assertIn("Developer ID Application identity", str(caught.exception))
        self.assertNotIn(invalid_identity, str(caught.exception))
        self.assertNotIn(secret_profile, str(caught.exception))

        with self.assertRaisesRegex(
            macos_packaging.PackagingError,
            "diagnostic mode refuses release credentials",
        ):
            macos_packaging.resolve_release_credentials(
                "diagnostic", secret_identity, secret_profile, {}
            )

        self.assertIsNone(
            macos_packaging.resolve_release_credentials(
                "diagnostic",
                None,
                None,
                {
                    "LINGTAI_CODESIGN_IDENTITY": secret_identity,
                    "LINGTAI_NOTARY_PROFILE": secret_profile,
                },
            )
        )

        credentials = macos_packaging.resolve_release_credentials(
            "release", secret_identity, secret_profile, {}
        )
        self.assertEqual(credentials.identity, secret_identity)
        self.assertEqual(credentials.notary_profile, secret_profile)

    def test_versioned_names_and_tool_output_parsers(self) -> None:
        names = macos_packaging.artifact_names("0.1.5")
        self.assertEqual(names.dmg, "LingTai-0.1.5-macOS-universal.dmg")
        self.assertEqual(names.manifest, "LingTai-0.1.5-macOS-universal.manifest.json")
        self.assertEqual(
            macos_packaging.parse_architectures("x86_64 arm64\n"),
            ("arm64", "x86_64"),
        )
        self.assertEqual(
            macos_packaging.parse_minos(
                """binary (architecture arm64):
    platform MACOS
      minos 13.0
binary (architecture x86_64):
    platform MACOS
      minos 13.0
"""
            ),
            {"arm64": "13.0", "x86_64": "13.0"},
        )
        for platform in ("MACCATALYST", "IOS", "TVOS"):
            with self.subTest(platform=platform), self.assertRaisesRegex(
                macos_packaging.PackagingError, "platform MACOS"
            ):
                macos_packaging.parse_minos(
                    f"""binary (architecture arm64):
    platform {platform}
      minos 13.0
"""
                )
            with self.assertRaisesRegex(
                verify_macos_package.VerificationError, "platform MACOS"
            ):
                verify_macos_package._parse_minos(
                    f"""binary (architecture arm64):
    platform {platform}
      minos 13.0
"""
                )
        with self.assertRaisesRegex(macos_packaging.PackagingError, "unsafe version"):
            macos_packaging.artifact_names("0.1.5/overwrite")

    def test_destination_refuses_app_ancestry_and_overwrite(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            app = root / "LingTai.app"
            app.mkdir()
            safe_output = root / "out"
            safe_output.mkdir()
            names = macos_packaging.artifact_names("0.1.5")

            dmg, manifest = macos_packaging.validate_output_paths(
                app, safe_output, names
            )
            self.assertEqual(dmg.parent, safe_output.resolve())
            self.assertEqual(manifest.parent, safe_output.resolve())

            with self.assertRaisesRegex(
                macos_packaging.PackagingError, "must not be inside the input App"
            ):
                macos_packaging.validate_output_paths(app, app / "artifacts", names)

            dmg.touch()
            with self.assertRaisesRegex(
                macos_packaging.PackagingError, "refusing to overwrite"
            ):
                macos_packaging.validate_output_paths(app, safe_output, names)

            dmg.unlink()
            manifest.touch()
            with self.assertRaisesRegex(
                macos_packaging.PackagingError, "refusing to overwrite"
            ):
                macos_packaging.validate_output_paths(app, safe_output, names)

    def test_pair_publication_is_exclusive_and_rolls_back_only_its_own_dmg(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            scratch = root / "scratch"
            output = root / "output"
            scratch.mkdir()
            output.mkdir()
            staged_dmg = scratch / "artifact.dmg"
            staged_manifest = scratch / "artifact.manifest.json"
            staged_dmg.write_bytes(b"complete-dmg")
            staged_manifest.write_bytes(b"complete-manifest")

            final_dmg = output / staged_dmg.name
            final_manifest = output / staged_manifest.name
            macos_packaging.publish_artifact_pair(
                staged_dmg, staged_manifest, final_dmg, final_manifest
            )
            self.assertEqual(final_dmg.read_bytes(), b"complete-dmg")
            self.assertEqual(final_manifest.read_bytes(), b"complete-manifest")

            final_dmg.unlink()
            final_manifest.unlink()
            final_dmg.write_bytes(b"late-dmg-racer")
            with self.assertRaisesRegex(
                macos_packaging.PackagingError, "DMG appeared during packaging"
            ):
                macos_packaging.publish_artifact_pair(
                    staged_dmg, staged_manifest, final_dmg, final_manifest
                )
            self.assertEqual(final_dmg.read_bytes(), b"late-dmg-racer")
            self.assertFalse(final_manifest.exists())

            final_dmg.unlink()
            final_manifest.write_bytes(b"late-manifest-racer")
            with self.assertRaisesRegex(
                macos_packaging.PackagingError, "manifest appeared during packaging"
            ):
                macos_packaging.publish_artifact_pair(
                    staged_dmg, staged_manifest, final_dmg, final_manifest
                )
            self.assertFalse(final_dmg.exists())
            self.assertEqual(final_manifest.read_bytes(), b"late-manifest-racer")

            real_link = os.link

            def replace_dmg_before_manifest_link(
                source: Path, destination: Path, **kwargs: object
            ) -> None:
                if Path(destination) == final_manifest:
                    final_dmg.unlink()
                    final_dmg.write_bytes(b"replacement-dmg-racer")
                real_link(source, destination, **kwargs)

            with mock.patch.object(
                macos_packaging.os,
                "link",
                side_effect=replace_dmg_before_manifest_link,
            ), self.assertRaisesRegex(
                macos_packaging.PackagingError, "manifest appeared during packaging"
            ):
                macos_packaging.publish_artifact_pair(
                    staged_dmg, staged_manifest, final_dmg, final_manifest
                )
            self.assertEqual(final_dmg.read_bytes(), b"replacement-dmg-racer")
            self.assertEqual(final_manifest.read_bytes(), b"late-manifest-racer")

    def test_pair_publication_rolls_back_on_ordinary_second_link_error(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            staged_dmg = root / "staged.dmg"
            staged_manifest = root / "staged.manifest.json"
            final_dmg = root / "out" / "final.dmg"
            final_manifest = root / "out" / "final.manifest.json"
            final_dmg.parent.mkdir()
            staged_dmg.write_bytes(b"complete-dmg")
            staged_manifest.write_bytes(b"complete-manifest")
            real_link = os.link

            def fail_second_link(source: Path, destination: Path, **kwargs: object) -> None:
                if Path(destination) == final_manifest:
                    raise PermissionError("injected publication failure")
                real_link(source, destination, **kwargs)

            with mock.patch.object(
                macos_packaging.os, "link", side_effect=fail_second_link
            ), self.assertRaisesRegex(
                macos_packaging.PackagingError, "artifact publication failed"
            ):
                macos_packaging.publish_artifact_pair(
                    staged_dmg, staged_manifest, final_dmg, final_manifest
                )
            self.assertFalse(final_dmg.exists())
            self.assertFalse(final_manifest.exists())

    def test_manifest_is_bounded_canonical_and_secret_free(self) -> None:
        manifest = macos_packaging.render_manifest(
            version="0.1.5",
            packaging_git_sha="84b86396e123e9635c026a18bfd0f765fde81310",
            packaging_git_tree="784d60a6cde0eec28b8befaab6a796a4ea3b8d9d",
            packaging_git_dirty=False,
            file_name="LingTai-0.1.5-macOS-universal.dmg",
            size_bytes=1234,
            sha256="a" * 64,
            architectures=("arm64", "x86_64"),
            minimum_macos="13.0",
            signing_state="ad-hoc diagnostic",
            notarization_state="not performed (diagnostic mode)",
        )
        data = json.loads(manifest)
        self.assertEqual(
            list(data),
            [
                "architectures",
                "file_name",
                "minimum_macos",
                "notarization",
                "packaging_git_dirty",
                "packaging_git_sha",
                "packaging_git_tree",
                "sha256",
                "signing",
                "size_bytes",
                "version",
            ],
        )
        self.assertLess(len(manifest), 1024)
        self.assertNotIn("identity", manifest.lower())
        self.assertNotIn("profile", manifest.lower())

    def test_packaging_git_provenance_is_tracked_only_and_release_fails_closed(self) -> None:
        sha = "84b86396e123e9635c026a18bfd0f765fde81310"
        tree = "784d60a6cde0eec28b8befaab6a796a4ea3b8d9d"
        with mock.patch.object(
            macos_packaging,
            "run_checked",
            side_effect=[sha + "\n", tree + "\n", " M README.md\n"],
        ) as run:
            facts = macos_packaging.packaging_git_facts(Path("/repo"))
        self.assertEqual(
            facts,
            macos_packaging.PackagingGitFacts(sha=sha, tree=tree, dirty=True),
        )
        self.assertIn("--untracked-files=no", run.call_args_list[2].args[0])

        for invalid in (
            facts,
            macos_packaging.PackagingGitFacts(None, tree, False),
            macos_packaging.PackagingGitFacts(sha, None, False),
            macos_packaging.PackagingGitFacts(sha, tree, None),
        ):
            with self.subTest(facts=invalid), self.assertRaisesRegex(
                macos_packaging.PackagingError, "clean packaging Git provenance"
            ):
                macos_packaging.validate_packaging_git_facts(invalid, release=True)
        macos_packaging.validate_packaging_git_facts(
            macos_packaging.PackagingGitFacts(sha, tree, False), release=True
        )

    def test_verifier_checks_otool_links_for_each_architecture(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary) / "LingTai.app"
            executable = app / "Contents" / "MacOS" / "LingTai"
            framework = app / "Contents" / "Frameworks" / "libGood.dylib"
            executable.parent.mkdir(parents=True)
            framework.parent.mkdir(parents=True)
            executable.touch()
            framework.touch()

            calls: list[tuple[str, ...]] = []

            def good_otool(arguments: list[object], **_: object) -> str:
                args = tuple(os.fspath(value) for value in arguments)
                calls.append(args)
                if "-l" in args:
                    return "cmd LC_RPATH\npath @executable_path/../Frameworks (offset 12)\n"
                if "-L" in args:
                    return f"{executable}:\n\t@rpath/libGood.dylib (compatibility version 1.0.0)\n"
                if "-D" in args:
                    return f"{executable}:\n"
                self.fail(f"unexpected otool call: {args}")

            with mock.patch.object(
                verify_macos_package, "run_checked", side_effect=good_otool
            ):
                for architecture in ("arm64", "x86_64"):
                    verify_macos_package._verify_binary_links(
                        executable,
                        app,
                        executable.parent,
                        ["@executable_path/../Frameworks"],
                        Path("/usr/bin/otool"),
                        architecture,
                    )

            for architecture in ("arm64", "x86_64"):
                for operation in ("-l", "-D", "-L"):
                    self.assertTrue(
                        any(
                            ("-arch", architecture, operation)
                            == call[1:4]
                            for call in calls
                        ),
                        f"missing {operation} for {architecture}",
                    )

    def test_verifier_rejects_a_bad_link_in_only_one_slice(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary) / "LingTai.app"
            executable = app / "Contents" / "MacOS" / "LingTai"
            framework = app / "Contents" / "Frameworks" / "libGood.dylib"
            executable.parent.mkdir(parents=True)
            framework.parent.mkdir(parents=True)
            executable.touch()
            framework.touch()

            def slice_specific_otool(arguments: list[object], **_: object) -> str:
                args = tuple(os.fspath(value) for value in arguments)
                architecture = args[2]
                if "-l" in args:
                    rpath = (
                        "@executable_path/../Frameworks"
                        if architecture == "arm64"
                        else "/private/developer/Qt/lib"
                    )
                    return f"cmd LC_RPATH\npath {rpath} (offset 12)\n"
                if "-L" in args:
                    dependency = (
                        "@rpath/libGood.dylib"
                        if architecture == "arm64"
                        else "/private/developer/Qt/lib/libBad.dylib"
                    )
                    return f"{executable}:\n\t{dependency} (compatibility version 1.0.0)\n"
                if "-D" in args:
                    return f"{executable}:\n"
                self.fail(f"unexpected otool call: {args}")

            with mock.patch.object(
                verify_macos_package,
                "run_checked",
                side_effect=slice_specific_otool,
            ):
                verify_macos_package._verify_binary_links(
                    executable,
                    app,
                    executable.parent,
                    ["@executable_path/../Frameworks"],
                    Path("/usr/bin/otool"),
                    "arm64",
                )
                with self.assertRaisesRegex(
                    verify_macos_package.VerificationError,
                    "absolute non-system rpath",
                ):
                    verify_macos_package._verify_binary_links(
                        executable,
                        app,
                        executable.parent,
                        ["@executable_path/../Frameworks"],
                        Path("/usr/bin/otool"),
                        "x86_64",
                    )

            def bad_dependency_only(arguments: list[object], **_: object) -> str:
                args = tuple(os.fspath(value) for value in arguments)
                architecture = args[2]
                if "-l" in args:
                    return "cmd LC_RPATH\npath @executable_path/../Frameworks (offset 12)\n"
                if "-L" in args:
                    dependency = (
                        "@rpath/libGood.dylib"
                        if architecture == "arm64"
                        else "/private/developer/Qt/lib/libBad.dylib"
                    )
                    return f"{executable}:\n\t{dependency} (compatibility version 1.0.0)\n"
                if "-D" in args:
                    return f"{executable}:\n"
                self.fail(f"unexpected otool call: {args}")

            with mock.patch.object(
                verify_macos_package,
                "run_checked",
                side_effect=bad_dependency_only,
            ):
                verify_macos_package._verify_binary_links(
                    executable,
                    app,
                    executable.parent,
                    ["@executable_path/../Frameworks"],
                    Path("/usr/bin/otool"),
                    "arm64",
                )
                with self.assertRaisesRegex(
                    verify_macos_package.VerificationError,
                    "absolute non-system dependency",
                ):
                    verify_macos_package._verify_binary_links(
                        executable,
                        app,
                        executable.parent,
                        ["@executable_path/../Frameworks"],
                        Path("/usr/bin/otool"),
                        "x86_64",
                    )

    def test_missing_tool_fails_without_searching_untrusted_paths(self) -> None:
        with self.assertRaisesRegex(
            macos_packaging.PackagingError, "required tool is unavailable"
        ):
            macos_packaging.require_tool(
                "definitely-not-a-real-packaging-tool", {"PATH": "/usr/bin:/bin"}
            )


if __name__ == "__main__":
    unittest.main()
