#!/usr/bin/env python3
"""Check the staged beta and run it once with an empty temporary profile."""
import hashlib
import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from verification_receipt import ROOT, source_fingerprint, qt_receipt


def run(*args, **kwargs):
    return subprocess.run(args, check=True, capture_output=True, text=True,
                          timeout=30, **kwargs).stdout


def verify(app, report, expected_source, test_xml):
    if source_fingerprint() != expected_source:
        raise ValueError("Source changed during build/verification; rebuild the candidate")
    tests = qt_receipt(test_xml)
    info = plistlib.loads((app / "Contents/Info.plist").read_bytes())
    channels = {"xyz.beamgrove.keefi.dev": ("development", "KEEFI Dev"),
                "xyz.beamgrove.keefi.local": ("production", "KEEFI")}
    if info.get("CFBundleIdentifier") not in channels:
        raise ValueError("Expected a KEEFI development or production channel")
    channel, application = channels[info["CFBundleIdentifier"]]
    if info.get("CFBundleExecutable") != application:
        raise ValueError("Bundle identity and executable channel disagree")
    executable = app / "Contents/MacOS" / info["CFBundleExecutable"]
    run("/usr/bin/codesign", "--verify", "--deep", "--strict", str(app))
    required = ["Contents/PlugIns/platforms/libqcocoa.dylib",
                "Contents/PlugIns/tls/libqsecuretransportbackend.dylib",
                "Contents/Frameworks/QtWebSockets.framework",
                "Contents/Resources/qt.conf"]
    for name in required:
        if not (app / name).exists():
            raise ValueError("Missing runtime component: " + name)

    magic = {b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf",
             b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
             b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca"}
    allowed = ("@rpath/", "@loader_path/", "@executable_path/",
               "/usr/lib/", "/System/Library/")
    binaries = []
    for path in sorted(app.rglob("*")):
        if path.is_symlink() or not path.is_file():
            continue
        with path.open("rb") as stream:
            if stream.read(4) not in magic:
                continue
        if "x86_64" not in run("/usr/bin/lipo", "-archs", str(path)).split():
            raise ValueError("Missing x86_64: " + str(path.relative_to(app)))
        for line in run("/usr/bin/otool", "-arch", "x86_64", "-L", str(path)).splitlines()[1:]:
            dependency = line.strip().split(" (", 1)[0]
            if not dependency.startswith(allowed):
                raise ValueError("External runtime dependency: " + dependency)
        commands = run("/usr/bin/otool", "-arch", "x86_64", "-l", str(path)).splitlines()
        for index, line in enumerate(commands):
            if line.strip() == "cmd LC_RPATH":
                rpath = commands[index + 2].strip().removeprefix("path ").split(" (", 1)[0]
                if not rpath.startswith(allowed):
                    raise ValueError("External runtime RPATH: " + rpath)
        binaries.append(str(path.relative_to(app)))
    if not binaries:
        raise ValueError("No Mach-O binaries found")

    # Never inherit a test platform override or open the user's default profile.
    environment = os.environ.copy()
    for key in ("QT_QPA_PLATFORM", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH"):
        environment.pop(key, None)
    with tempfile.TemporaryDirectory(prefix="keefi-g5-empty-") as temporary:
        smoke = Path(temporary) / "smoke.json"
        run(str(executable), "--profile-dir", str(Path(temporary) / "profile"),
            "--smoke-report", str(smoke), env=environment)
        result = json.loads(smoke.read_text())
        expected = {"read_only_build": True, "build_channel": channel, "application": application,
                    "visible": True, "account_count": 0, "vault_exists": False,
                    "vault_unlocked": False, "security_ready": True}
        for key, value in expected.items():
            if result.get(key) != value:
                raise ValueError("Unexpected smoke result: " + key)
    git_head = None
    if (ROOT / ".git").exists():
        git_head = run("git", "-C", str(ROOT), "rev-parse", "HEAD").strip()
    receipt = {"bundle_version": info["CFBundleVersion"],
               "short_version": info["CFBundleShortVersionString"],
               "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
               "macho_files": binaries, "smoke": result,
               "verified_at_utc": datetime.now(timezone.utc).isoformat(),
               "source_fingerprint_sha256": expected_source,
               "git_head": git_head,
               "source_state": "working tree; fingerprint includes uncommitted source and tests",
               "tests": tests, "financial_accounts_used": False,
               "scope": "local staging; no installation or publisher authentication"}
    if source_fingerprint() != expected_source:
        raise ValueError("Source changed during verification")
    report.write_text(json.dumps(receipt, ensure_ascii=False, indent=2) + "\n")
    print(f"Verified {len(binaries)} Mach-O files, local signature and empty-profile startup.")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit("usage: verify-readonly-macos.py APP REPORT_JSON SOURCE_SHA256 TEST_XML")
    verify(Path(sys.argv[1]).resolve(strict=True), Path(sys.argv[2]), sys.argv[3], Path(sys.argv[4]))
