#!/usr/bin/env python3
"""Deterministic, offline contracts for the macOS packaging boundary."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from scripts import macos_packaging


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

    def test_manifest_is_bounded_canonical_and_secret_free(self) -> None:
        manifest = macos_packaging.render_manifest(
            version="0.1.5",
            git_sha="84b86396e123e9635c026a18bfd0f765fde81310",
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
                "git_sha",
                "minimum_macos",
                "notarization",
                "sha256",
                "signing",
                "size_bytes",
                "version",
            ],
        )
        self.assertLess(len(manifest), 1024)
        self.assertNotIn("identity", manifest.lower())
        self.assertNotIn("profile", manifest.lower())

    def test_missing_tool_fails_without_searching_untrusted_paths(self) -> None:
        with self.assertRaisesRegex(
            macos_packaging.PackagingError, "required tool is unavailable"
        ):
            macos_packaging.require_tool(
                "definitely-not-a-real-packaging-tool", {"PATH": "/usr/bin:/bin"}
            )


if __name__ == "__main__":
    unittest.main()
