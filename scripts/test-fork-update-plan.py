#!/usr/bin/env python3
"""Regression tests for upstream release planning."""

import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "planner", Path(__file__).with_name("plan-fork-update.py"))
planner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(planner)


class PlanTests(unittest.TestCase):
    @patch.object(planner.subprocess, "check_output", return_value="abc\n")
    @patch.object(planner, "checksums")
    @patch.object(planner, "gh")
    def test_modern_release_uses_mirror_and_separate_update_names(self, gh, sums, git):
        gh.side_effect = [
            [{"tagName": "v1.5"}],
            {"assets": [{"name": "SHA256SUMS"}, {"name": "zaviya-installer-0.1.0-beta.apk"}]},
            [],
        ]
        sums.return_value = f'{"a" * 64}  zaviya-sm8550-1.5.0.img.zst\n'
        outputs, message = planner.plan("upstream/repo", "fork/repo")
        self.assertEqual(outputs["prefix"], "zaviya-arm")
        self.assertEqual(outputs["upstream_kernel"], "true")
        self.assertEqual(outputs["tag"], "v1.5.1")
        self.assertIn("https://downloads.zaviyaos.workers.dev/1.5/", outputs["images"])
        self.assertEqual(outputs["socs"], '["sm8550"]')

    @patch.object(planner, "checksums", return_value="invalid manifest")
    def test_modern_release_without_verified_image_fails(self, sums):
        with self.assertRaisesRegex(ValueError, "SHA256SUMS"):
            planner.release_images("u/r", "v1.5", {"assets": []}, {"sm8550"})

    @patch.object(planner, "checksums")
    def test_attached_modern_image_does_not_use_mirror(self, sums):
        name = "zaviya-sm8550-1.6.0.img.zst"
        sums.return_value = f'{"a" * 64} *{name}\n'
        images, _, _ = planner.release_images(
            "u/r", "v1.6", {"assets": [{"name": name}]}, {"sm8550"})
        self.assertEqual(images[0]["url"], "")

    @patch.object(planner, "checksums")
    def test_manifest_paths_and_unknown_chips_are_not_downloaded(self, sums):
        sums.return_value = (
            f'{"a" * 64}  ../zaviya-sm8550-1.5.0.img.zst\n'
            f'{"b" * 64}  zaviya-sm9999-1.5.0.img.zst\n')
        with self.assertRaises(ValueError):
            planner.release_images("u/r", "v1.5", {"assets": []}, {"sm8550"})

    @patch.object(planner.subprocess, "check_output", return_value="abc\n")
    @patch.object(planner, "gh")
    def test_legacy_images_build_next_numeric_revision(self, gh, git):
        gh.side_effect = [
            {"assets": [{"name": name} for name in [
                "steamos-arm-port-sm8550-v1.4.7z.001",
                "steamos-arm-port-sm8550-v1.4.7z.002",
                "steamos-arm-port-sm8650-v1.4.7z.001",
                "steamos-arm-port-sm9999-v1.4.7z.001",
            ]]},
            [{"tagName": tag} for tag in ["v1.4.2", "v1.4.10", "v1.40.99", "fork-assets"]],
            {"body": "Built from main@old"},
        ]
        outputs, _ = planner.plan("upstream/repo", "fork/repo", "v1.4")
        self.assertEqual(outputs["tag"], "v1.4.11")
        self.assertEqual(outputs["socs"], '["sm8550", "sm8650"]')
        self.assertEqual(outputs["head"], "abc")
        self.assertEqual(outputs["build"], "true")
        self.assertEqual(outputs["prefix"], "steamos-arm")
        self.assertEqual(outputs["upstream_kernel"], "false")

    @patch.object(planner.subprocess, "check_output", return_value="abc\n")
    @patch.object(planner, "gh")
    def test_unchanged_release_skips_unless_rebuild_requested(self, gh, git):
        assets = {"assets": [{"name": "steamos-arm-port-sm8550-v1.4.7z.001"}]}
        releases = [{"tagName": "v1.4.1"}]
        gh.side_effect = [assets, releases, {"body": "Built from main@abc"}]
        self.assertEqual(planner.plan("u/r", "f/r", "v1.4")[0], {"build": "false"})
        gh.side_effect = [assets, releases]
        self.assertEqual(planner.plan("u/r", "f/r", "v1.4", True)[0]["tag"], "v1.4.2")

    @patch.object(planner, "gh", side_effect=RuntimeError("API unavailable"))
    def test_api_errors_are_not_silently_skipped(self, gh):
        with self.assertRaisesRegex(RuntimeError, "API unavailable"):
            planner.plan("u/r", "f/r")

    def test_invalid_tag_is_rejected(self):
        with self.assertRaises(ValueError):
            planner.plan("u/r", "f/r", "v1.5-beta")


if __name__ == "__main__":
    unittest.main()
