#!/usr/bin/env python3
"""Tests for reconciliation verdicts, including differences hidden by equal totals."""
import copy
import tempfile
import unittest
from pathlib import Path

import compare
import recalc
from test_recalc import c, run, s, T


def bundle(snapshots=None):
    if snapshots is None:
        snapshots = [s([c("cash", "KRW", "1000", kind="Cash"), c("zero", "ZERO", "0")])]
    expected = run(snapshots)
    app = copy.deepcopy(expected)
    app["rows"] = []
    for group in app["groups"]:
        for row in group.pop("rows"):
            row["identity"] = group["identity"]
            row["asset"] = group["asset"]
            app["rows"].append(row)
    app["allocation_total"] = app["allocation"]["total"]
    return {"format": recalc.FORMAT, "as_of_ms": T,
            "connections": [{"id": "c1", "environment": "live", "state": "complete"}],
            "snapshots": snapshots, "quotes": [], "reference_quotes": [], "keefi_result": app}


class Comparison(unittest.TestCase):
    def test_matches_all_semantic_fields_without_changing_inputs(self):
        data = bundle()
        before = copy.deepcopy(data)
        result = compare.compare_bundle(data)
        self.assertTrue(result["matched"])
        self.assertGreater(result["checks"], 30)
        self.assertEqual(data, before)

    def test_equal_total_does_not_hide_changed_row_values(self):
        data = bundle()
        for row in data["keefi_result"]["rows"]:
            row["value"] = "999" if row["component_id"] == "cash" else "1"
        result = compare.compare_bundle(data)
        self.assertFalse(result["matched"])
        self.assertEqual(len([d for d in result["differences"] if d["path"].endswith("/value")]), 2)

    def test_missing_keefi_output_is_an_error_not_a_pass(self):
        data = bundle()
        del data["keefi_result"]
        with self.assertRaises(ValueError):
            compare.compare_bundle(data)

    def test_missing_required_field_is_an_error_even_for_zero(self):
        data = bundle()
        del data["keefi_result"]["rows"][0]["state"]
        with self.assertRaises(ValueError):
            compare.compare_bundle(data)

    def test_extra_duplicate_row_is_detected_instead_of_overwritten(self):
        data = bundle()
        data["keefi_result"]["rows"].append(copy.deepcopy(data["keefi_result"]["rows"][0]))
        result = compare.compare_bundle(data)
        self.assertFalse(result["matched"])
        self.assertTrue(any(d["path"] == "rows/identities" for d in result["differences"]))

    def test_policy_duplicate_rows_remain_separately_comparable(self):
        data = bundle([s([c("same", "KRW", "1000", kind="Cash"), c("same", "KRW", "50", kind="Cash")])])
        self.assertTrue(compare.compare_bundle(data)["matched"])
        data["keefi_result"]["rows"][1]["state"] = "Fresh"
        self.assertFalse(compare.compare_bundle(data)["matched"])

    def test_does_not_copy_raw_responses_into_the_report(self):
        data = bundle()
        data["raw_responses"] = {"entries": [{"body_text": "FAKE_PRIVATE_RAW_RESPONSE"}]}
        self.assertNotIn("FAKE_PRIVATE_RAW_RESPONSE", str(compare.compare_bundle(data)))

    def test_decimal_representation_does_not_create_a_false_difference(self):
        data = bundle()
        data["keefi_result"]["valuation"]["known_subtotal"] = "1000.000"
        self.assertTrue(compare.compare_bundle(data)["matched"])

    def test_report_is_private_and_never_overwrites_an_existing_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "report.json"
            compare.write_report(path, {"matched": True})
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            with self.assertRaises(FileExistsError):
                compare.write_report(path, {"matched": False})
            self.assertIn('"matched": true', path.read_text())


if __name__ == "__main__":
    unittest.main()
