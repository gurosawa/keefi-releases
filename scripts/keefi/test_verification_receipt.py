#!/usr/bin/env python3
"""Synthetic XML tests for the current read-only installation gate."""
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

from verification_receipt import qt_receipt

CURRENT = (
    "accountScreenRecoversFailedBrokerAuthentication", "providerReadsKeepLastSnapshotOnFailure",
    "validMutationsFollowBuildPolicy", "cooldownIsSharedAcrossKeysAndTransports",
    "exhaustedPublicQuotaAndInvalidRetryAfterAreConservative", "readRoutesRemainBoundToTheirPayloads",
    "accountReadAndHomeEntry", "swapExecutionRoutesAreAbsent",
    "addressValidationPreservesReadOnlyCompatibility", "exchangeRequestBuildersCannotConstructFinancialWrites",
)


class ReadOnlyReceiptContract(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "synthetic.xml"

    def write_cases(self, names):
        root = ET.Element("TestCase", name="ReadOnlyTest")
        for name in ("initTestCase", *names, "cleanupTestCase"):
            function = ET.SubElement(root, "TestFunction", name=name)
            ET.SubElement(function, "Incident", type="pass")
        ET.ElementTree(root).write(self.path, encoding="utf-8")
        return root

    def test_current_query_only_contract_is_accepted(self):
        self.write_cases(CURRENT)
        result = qt_receipt(self.path)
        self.assertEqual(result["actual_cases"], len(CURRENT))
        self.assertEqual(result["passed"], len(CURRENT) + 2)
        self.assertEqual(result["evidence_kind"], "synthetic")

    def test_each_required_contract_is_mandatory(self):
        for missing in CURRENT:
            with self.subTest(missing=missing):
                self.write_cases(tuple(name for name in CURRENT if name != missing))
                with self.assertRaises(ValueError):
                    qt_receipt(self.path)

    def test_former_guard_only_receipt_is_not_current_removal_evidence(self):
        old = (*CURRENT[:5], "signingAndSwapSubmissionFollowBuildPolicy")
        self.write_cases(old)
        with self.assertRaises(ValueError):
            qt_receipt(self.path)

    def test_duplicate_or_skipped_incidents_do_not_pass(self):
        self.write_cases((*CURRENT, CURRENT[0]))
        with self.assertRaises(ValueError):
            qt_receipt(self.path)
        root = self.write_cases(CURRENT)
        ET.SubElement(root.find("TestFunction"), "Message", type="skip")
        ET.ElementTree(root).write(self.path, encoding="utf-8")
        with self.assertRaises(ValueError):
            qt_receipt(self.path)


if __name__ == "__main__":
    unittest.main()
