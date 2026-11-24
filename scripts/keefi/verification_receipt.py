#!/usr/bin/env python3
"""Bind synthetic Qt verification to a source fingerprint; never read profiles."""
import hashlib
from pathlib import Path
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]


def source_fingerprint(root=ROOT):
    root = Path(root).resolve(strict=True)
    paths = set()
    for relative in ("fincept-qt/src/keefi", "fincept-qt/cmake",
                     "fincept-qt/tests/keefi", "scripts/keefi", "scripts/keefi-recon"):
        directory = root / relative
        if not directory.is_dir() or directory.is_symlink():
            raise ValueError("Missing source fingerprint directory: " + relative)
        selected = {p for p in directory.rglob("*")
                    if p.is_file() and not p.is_symlink() and "__pycache__" not in p.parts}
        if not selected:
            raise ValueError("Empty source fingerprint directory: " + relative)
        paths.update(selected)
    cmake = root / "fincept-qt/CMakeLists.txt"
    if not cmake.is_file() or cmake.is_symlink():
        raise ValueError("Missing source fingerprint CMake entrypoint")
    paths.add(cmake)
    resources = sorted((root / "fincept-qt/resources").glob("keefi*.qrc"))
    required = {"keefi_fonts.qrc", "keefi_notices.qrc", "keefi_phrases.qrc"}
    if not required.issubset({p.name for p in resources}):
        raise ValueError("Missing KEEFI font, notice or phrase resource manifest")
    for resource in resources:
        if resource.is_symlink():
            raise ValueError("Resource manifest must be a regular checkout file")
        paths.add(resource)
        nodes = list(ET.parse(resource).getroot().iter("file"))
        if not nodes:
            raise ValueError("Empty resource manifest: " + resource.name)
        for node in nodes:
            if not node.text:
                raise ValueError("Resource entry has no source path")
            dependency = (resource.parent / node.text).resolve(strict=True)
            dependency.relative_to(root)  # Source resources remain inside this checkout.
            if not dependency.is_file():
                raise ValueError("Resource dependency is not a regular file")
            paths.add(dependency)
    digest = hashlib.sha256()
    for path in sorted(paths):
        digest.update(path.relative_to(root).as_posix().encode() + b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def qt_receipt(path):
    document = ET.parse(path).getroot()
    cases, lifecycle = [], 0
    for function in document.findall("TestFunction"):
        name = function.attrib["name"]
        incidents = function.findall("Incident")
        if not incidents or function.findall("Message[@type='skip']"):
            raise ValueError("Missing or skipped test: " + name)
        for incident in incidents:
            if incident.attrib.get("type") != "pass":
                raise ValueError("Non-passing test: " + name)
            tag = incident.findtext("DataTag", "")
            if name in ("initTestCase", "cleanupTestCase"):
                lifecycle += 1
            else:
                cases.append(name + (":" + tag if tag else ""))
    required = {"accountScreenRecoversFailedBrokerAuthentication", "providerReadsKeepLastSnapshotOnFailure",
                "validMutationsFollowBuildPolicy", "cooldownIsSharedAcrossKeysAndTransports",
                "exhaustedPublicQuotaAndInvalidRetryAfterAreConservative", "readRoutesRemainBoundToTheirPayloads",
                "accountReadAndHomeEntry", "swapExecutionRoutesAreAbsent",
                "addressValidationPreservesReadOnlyCompatibility", "exchangeRequestBuildersCannotConstructFinancialWrites"}
    if not required.issubset({case.split(":", 1)[0] for case in cases}) or lifecycle != 2:
        raise ValueError("Incomplete current read-only removal verification run")
    if len(cases) != len(set(cases)):
        raise ValueError("Duplicate Qt case identity")
    return {"contract": "keefi-query-only/2", "evidence_kind": "synthetic", "actual_cases": len(cases), "qt_lifecycle_cases": lifecycle,
            "passed": len(cases) + lifecycle, "failed": 0, "cases": cases,
            "xml_sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


if __name__ == "__main__":
    if sys.argv[1:] != ["--fingerprint"]:
        raise SystemExit("usage: verification_receipt.py --fingerprint")
    print(source_fingerprint())
