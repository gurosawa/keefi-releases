#!/usr/bin/env python3
"""Synthetic guard regressions for the KEEFI build and Dev101 gate."""
import hashlib
from pathlib import Path
import tempfile
import unittest

from gate_contract import (approved_format_document_record, normalized_rebrand_bytes,
                           protected_rebrand_record, registered_test_preservation,
                           required_test_names)
from test_g8_theme_source import violations
from verification_receipt import source_fingerprint


class RegisteredSuites(unittest.TestCase):
    def test_all_179_old_suites_map_once_and_new_suites_are_separate(self):
        current = sorted(required_test_names()) + ["keefi_dev101_added"]
        result = registered_test_preservation(current)
        self.assertEqual(result["baseline_count"], 179)
        self.assertEqual(result["mapped_count"], 179)
        self.assertEqual(result["registered_count"], 180)
        self.assertEqual(result["additional"], ["keefi_dev101_added"])
        self.assertEqual(len({row["legacy"] for row in result["mapping"]}), 179)
        self.assertEqual(len({row["current"] for row in result["mapping"]}), 179)

    def test_each_original_suite_is_required(self):
        required = sorted(required_test_names())
        for missing in required:
            with self.subTest(missing=missing):
                with self.assertRaisesRegex(ValueError, "Missing migrated"):
                    registered_test_preservation(name for name in required if name != missing)

    def test_duplicate_or_legacy_registration_cannot_replace_a_suite(self):
        current = sorted(required_test_names())
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            registered_test_preservation(current + current[:1])
        with self.assertRaisesRegex(ValueError, "KEEFI registration"):
            registered_test_preservation(current + ["finix_extra"])


class ProtectedSource(unittest.TestCase):
    def test_only_exact_three_name_substitutions_are_allowed(self):
        old = b'#include "finix/FinixTheme.h"\nnamespace finix { const char* id = "FINIX"; }\n'
        new = b'#include "keefi/KeefiTheme.h"\nnamespace keefi { const char* id = "KEEFI"; }\n'
        self.assertEqual(normalized_rebrand_bytes(old), new)
        record = protected_rebrand_record(old, new)
        self.assertTrue(record["identical_after_approved_substitutions"])
        self.assertEqual(record["baseline_sha256"], hashlib.sha256(old).hexdigest())
        self.assertEqual(record["sha256"], hashlib.sha256(new).hexdigest())

    def test_calculation_and_whitespace_changes_are_rejected(self):
        old = b"namespace finix { double ratio(double x) { return x * 100.0; } }\n"
        new = normalized_rebrand_bytes(old)
        for invalid in (new.replace(b"100.0", b"10.0"), new + b"\n", old):
            with self.subTest(candidate=invalid):
                with self.assertRaisesRegex(ValueError, "Protected source differs"):
                    protected_rebrand_record(old, invalid)

    def test_new_format_document_needs_authorization_and_exact_bytes(self):
        old, new = b"legacy synthetic format", b"new synthetic format"
        sha = hashlib.sha256(new).hexdigest()
        result = approved_format_document_record(old, new, sha, "synthetic explicit format approval")
        self.assertTrue(result["approved_storage_format_transition"])
        self.assertFalse(result["unchanged_from_baseline"])
        for digest, approval, contents in (("", "approved", new), (sha, "", new),
                                            (sha, "approved", new + b"changed")):
            with self.subTest(digest=digest, approval=approval):
                with self.assertRaises(ValueError):
                    approved_format_document_record(old, contents, digest, approval)


class SourceInspection(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="keefi-build-contract-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.directories = ("fincept-qt/src/keefi", "fincept-qt/cmake",
                            "fincept-qt/tests/keefi", "scripts/keefi", "scripts/keefi-recon")
        for relative in self.directories:
            directory = self.root / relative
            directory.mkdir(parents=True)
            (directory / "synthetic.cpp").write_text("// synthetic fixture\n")
        (self.root / "fincept-qt/CMakeLists.txt").write_text("# synthetic CMake\n")
        self.resources = self.root / "fincept-qt/resources"
        self.resources.mkdir()
        (self.resources / "synthetic.txt").write_text("synthetic resource")
        for name in ("keefi_fonts.qrc", "keefi_notices.qrc", "keefi_phrases.qrc"):
            (self.resources / name).write_text(
                '<RCC><qresource><file>synthetic.txt</file></qresource></RCC>')

    def test_fingerprint_changes_with_source_recon_or_resource_bytes(self):
        before = source_fingerprint(self.root)
        for relative in ("fincept-qt/src/keefi/synthetic.cpp", "scripts/keefi-recon/synthetic.cpp",
                         "fincept-qt/resources/synthetic.txt"):
            path = self.root / relative
            original = path.read_bytes()
            path.write_bytes(original + b"changed")
            self.assertNotEqual(source_fingerprint(self.root), before)
            path.write_bytes(original)
            self.assertEqual(source_fingerprint(self.root), before)

    def test_each_required_source_directory_must_exist_and_be_nonempty(self):
        for relative in self.directories:
            directory = self.root / relative
            file = directory / "synthetic.cpp"
            file.unlink()
            with self.subTest(relative=relative, state="empty"):
                with self.assertRaisesRegex(ValueError, "Empty source"):
                    source_fingerprint(self.root)
            directory.rmdir()
            with self.subTest(relative=relative, state="missing"):
                with self.assertRaisesRegex(ValueError, "Missing source"):
                    source_fingerprint(self.root)
            directory.mkdir()
            file.write_text("// synthetic fixture\n")

    def test_each_resource_manifest_is_required_and_cannot_be_empty(self):
        for name in ("keefi_fonts.qrc", "keefi_notices.qrc", "keefi_phrases.qrc"):
            path = self.resources / name
            original = path.read_text()
            path.unlink()
            with self.subTest(manifest=name, state="missing"):
                with self.assertRaisesRegex(ValueError, "Missing KEEFI"):
                    source_fingerprint(self.root)
            path.write_text("<RCC><qresource/></RCC>")
            with self.subTest(manifest=name, state="empty"):
                with self.assertRaisesRegex(ValueError, "Empty resource"):
                    source_fingerprint(self.root)
            path.write_text(original)

    def test_resource_dependency_cannot_escape_checkout(self):
        with tempfile.TemporaryDirectory(prefix="keefi-outside-fixture-") as outside:
            dependency = Path(outside) / "synthetic.txt"
            dependency.write_text("outside fixture")
            (self.resources / "keefi_fonts.qrc").write_text(
                "<RCC><qresource><file>" + str(dependency) + "</file></qresource></RCC>")
            with self.assertRaises(ValueError):
                source_fingerprint(self.root)

    def test_theme_missing_or_empty_tree_fails_and_real_violation_is_found(self):
        directory = self.root / "fincept-qt/src/keefi"
        file = directory / "synthetic.cpp"
        self.assertEqual(violations(self.root), [])
        file.write_text('QColor value("#ff0000");\n')
        result = violations(self.root)
        self.assertTrue(any(row["kind"] == "hex_colour" for row in result))
        file.unlink()
        with self.assertRaisesRegex(ValueError, "Empty KEEFI"):
            violations(self.root)
        directory.rmdir()
        with self.assertRaisesRegex(ValueError, "Missing KEEFI"):
            violations(self.root)


if __name__ == "__main__":
    unittest.main()
