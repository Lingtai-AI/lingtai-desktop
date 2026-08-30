#!/usr/bin/env python3
"""Static safety contract for Desktop-owned project staging rollback."""

from __future__ import annotations

import hashlib
import unittest
from pathlib import Path


SOURCE = Path(__file__).resolve().parents[1] / "src" / "project_creation.cpp"
CONTENT_SOURCE = (
    Path(__file__).resolve().parents[1] / "src" / "project_creation_resources.cpp"
)
CMAKE = Path(__file__).resolve().parents[1] / "CMakeLists.txt"
ADAPTIVE_FIXTURE = (
    Path(__file__).resolve().parent
    / "resources/project_creation"
    / "adaptive-ead292d48703192c31f0abda791a666ffc6c0263"
)
EXPECTED_ASSET_SHA256 = {
    ".recipe/comment/comment.md": "ad80d0bd016b716b0331c898623557c0c116c5f7e9a84bfdfdc7761e48982ff5",
    ".recipe/comment/wen/comment.md": "ad80d0bd016b716b0331c898623557c0c116c5f7e9a84bfdfdc7761e48982ff5",
    ".recipe/comment/zh/comment.md": "ad80d0bd016b716b0331c898623557c0c116c5f7e9a84bfdfdc7761e48982ff5",
    ".recipe/greet/greet.md": "31474dddd74a2d78642ceb20fe4f2b678ec2d9e197e6b361f5ccbad738efbbb5",
    ".recipe/greet/wen/greet.md": "d90072d157a2b365a12559f5240cd34b1f4f8d64702589816f89fc5d4e06fdbf",
    ".recipe/greet/zh/greet.md": "a71488159336785e1b030a7b2b127c844af2010e869d3fbccadec62869a26269",
    ".recipe/recipe.json": "cdadde9aea1075d58ea669d5e8fdafea95cff3d0d6096fbfe31b64abedc2965c",
}


class ProjectCreationSourceContractTest(unittest.TestCase):
    def test_rollback_is_descriptor_relative_not_path_recursive(self) -> None:
        implementation = SOURCE.read_text()
        self.assertNotIn("remove_all", implementation)
        for primitive in ("::openat", "::fstatat", "AT_SYMLINK_NOFOLLOW", "::unlinkat"):
            self.assertIn(primitive, implementation)

    def test_creation_has_no_tui_or_runtime_readiness_adapter(self) -> None:
        self.assertTrue(
            CONTENT_SOURCE.is_file(),
            "Desktop-owned localized project-creation resources are missing",
        )
        implementation = SOURCE.read_text()
        for forbidden in (
            "project_create.go",
            "tui/internal",
            "QProcess",
            "runtime_python_available",
        ):
            self.assertNotIn(forbidden, implementation)
        self.assertIn("src/project_creation_resources.cpp", CMAKE.read_text())

    def test_adaptive_fixture_is_the_exact_pinned_seven_file_set(self) -> None:
        actual_assets = {
            path.relative_to(ADAPTIVE_FIXTURE).as_posix()
            for path in ADAPTIVE_FIXTURE.rglob("*")
            if path.is_file() and path.name not in {"PROVENANCE.md", "SHA256SUMS"}
        }
        self.assertEqual(actual_assets, set(EXPECTED_ASSET_SHA256))
        for relative, expected in EXPECTED_ASSET_SHA256.items():
            digest = hashlib.sha256(
                (ADAPTIVE_FIXTURE / relative).read_bytes()
            ).hexdigest()
            self.assertEqual(digest, expected, relative)
        resources = CONTENT_SOURCE.read_text()
        self.assertIn("ead292d48703192c31f0abda791a666ffc6c0263", resources)

    def test_guidance_has_no_network_or_global_state_boundary(self) -> None:
        implementation = SOURCE.read_text()
        header = SOURCE.with_suffix(".h").read_text()
        resources = CONTENT_SOURCE.read_text()
        for forbidden in (
            "QNetworkAccessManager",
            "QNetworkRequest",
            "QNetworkReply",
            "ResolveLocation",
            "ipinfo.io",
            "getaddrinfo",
            "std::getenv",
            "qgetenv",
            "::setenv",
            "::putenv",
            "registry.jsonl",
            "commands.json",
            '".recipe"',
            '".tui-asset"',
        ):
            self.assertNotIn(forbidden, implementation)
        self.assertIn("guidance_local_time", header)
        self.assertIn("guidance_cached_location", header)
        self.assertIn("QDateTime::currentDateTime()", implementation)
        self.assertNotIn("currentDateTimeUtc", implementation)
        for forbidden_guidance in (
            "tui/internal/",
            "update-tui",
            "`/viz`",
            "`/mcp`",
            "`/settings`",
            "`/doctor`",
            "`/projects`",
            "`/skills`",
            "`/nirvana`",
            "`/secretary`",
            "`/brief`",
            "ctrl+e",
            "Option+click",
        ):
            self.assertNotIn(forbidden_guidance, resources)


if __name__ == "__main__":
    unittest.main()
