#!/usr/bin/env python3
"""Keep full-bundle comparisons sensitive to bytes, names, links and modes."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("reproducible_macos", Path(__file__).with_name("reproducible_macos.py"))
repro = importlib.util.module_from_spec(spec)
spec.loader.exec_module(repro)


class BundleComparison(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.a = Path(self.temporary.name) / "a.app"
        self.b = Path(self.temporary.name) / "b.app"
        for root in (self.a, self.b):
            (root / "Contents").mkdir(parents=True)
            (root / "Contents/file").write_bytes(b"same content")

    def test_identical_contents_ignore_mtime(self):
        os.utime(self.a / "Contents/file", (1700000000, 1700000000))
        os.utime(self.b / "Contents/file", (1750000000, 1750000000))
        self.assertTrue(repro.compare(self.a, self.b)["equal"])

    def test_same_size_changed_bytes_are_detected(self):
        (self.b / "Contents/file").write_bytes(b"same contenX")
        result = repro.compare(self.a, self.b)
        self.assertFalse(result["equal"])
        self.assertEqual([x["path"] for x in result["differences"]], ["Contents/file"])

    def test_extra_entry_is_detected(self):
        (self.b / "Contents/extra").write_bytes(b"extra")
        self.assertFalse(repro.compare(self.a, self.b)["equal"])

    def test_permission_change_is_detected(self):
        (self.a / "Contents/file").chmod(0o644)
        (self.b / "Contents/file").chmod(0o600)
        result = repro.compare(self.a, self.b)
        self.assertFalse(result["equal"])
        self.assertEqual(result["differences"][0]["left"]["mode"], "0644")
        self.assertEqual(result["differences"][0]["right"]["mode"], "0600")

    def test_bundle_root_permissions_are_part_of_the_manifest(self):
        self.a.chmod(0o700)
        self.b.chmod(0o755)
        result = repro.compare(self.a, self.b)
        self.assertFalse(result["equal"])
        self.assertEqual([item["path"] for item in result["differences"]], ["."])
        self.assertEqual(result["differences"][0]["left"]["mode"], "0700")
        self.assertEqual(result["differences"][0]["right"]["mode"], "0755")

    def test_symlinks_compare_targets_without_reading_them(self):
        # Nonexistent destinations prove that manifest does not follow them.
        (self.a / "Contents/link").symlink_to("missing-a")
        (self.b / "Contents/link").symlink_to("missing-b")
        self.assertFalse(repro.compare(self.a, self.b)["equal"])
        self.assertEqual(repro.manifest(self.a)["Contents/link"]["target"], "missing-a")

    def test_epoch_is_positive_and_does_not_change_parent_environment(self):
        original = os.environ.get("SOURCE_DATE_EPOCH")
        for invalid in ("0", "-1", "1.2", "tomorrow", "253402300800", "١٧٠٠٠٠٠٠٠٠"):
            with self.assertRaises(ValueError):
                repro.reproducible_environment(invalid)
        environment = repro.reproducible_environment("1700000000")
        self.assertEqual(environment["SOURCE_DATE_EPOCH"], environment["QT_RCC_SOURCE_DATE_OVERRIDE"])
        self.assertEqual(os.environ.get("SOURCE_DATE_EPOCH"), original)


class ChannelNames(unittest.TestCase):
    def test_both_channels_use_keefi_bundle_and_target_names(self):
        self.assertEqual(repro.CHANNELS, {"KEEFI": "KEEFI.app", "KEEFI_Dev": "KEEFI Dev.app"})


if __name__ == "__main__":
    unittest.main()
