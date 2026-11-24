"""Run the macOS installer against an isolated older app and an encrypted synthetic profile."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import signal
import subprocess
import tempfile
import time
import unicodedata
import zipfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stored_bytes(profile):
    return {name: sha(profile / name) for name in ("credentials.keefi-vault", ".keefi-profile.json")}


def build(app):
    return plistlib.loads((app / "Contents/Info.plist").read_bytes())["CFBundleVersion"]


def running(executable):
    # LaunchServices normalizes file names on macOS.
    command = unicodedata.normalize("NFC", str(executable)) + " "
    listing = subprocess.check_output(["/bin/ps", "-axo", "pid=,command="], text=True)
    matches = {}
    for line in listing.splitlines():
        fields = unicodedata.normalize("NFC", line.strip()).split(None, 1)
        if len(fields) == 2 and fields[1].startswith(command):
            matches[int(fields[0])] = fields[1]
    return matches


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check(old_bundle, archive, probe):
    archive = archive.resolve(strict=True)
    probe = probe.resolve(strict=True)
    scripts = Path(__file__).resolve().parents[2] / "fincept-qt/resources/keefi/update"
    with zipfile.ZipFile(archive) as package:
        meta = plistlib.loads(package.read("KEEFI.app/Contents/Info.plist"))
    expected = str(meta["CFBundleVersion"])
    previous = build(old_bundle)
    require(int(expected) > int(previous), "Select an older installed app and a newer ZIP")
    digest = sha(archive)
    with tempfile.TemporaryDirectory(prefix="KEEFI upgrade ' ") as tmp:
        root = Path(tmp).resolve()
        app = root / "KEEFI.app"
        exe = app / "Contents/MacOS/KEEFI"
        profile = root / "검증용 금고 '"
        subprocess.run(["/usr/bin/ditto", old_bundle, app], check=True)
        subprocess.run([probe, "seed", profile], check=True)
        before = stored_bytes(profile)
        # Refuse a changed ZIP before touching the installed bundle.
        broken = root / "changed.zip"
        broken.write_bytes(b"changed")
        old_hash = sha(exe)
        args = [digest, meta["CFBundleShortVersionString"], expected, app, "user", str(os.getuid())]
        refused = subprocess.run(["/bin/sh", scripts / "replace.sh", broken, *args, profile], capture_output=True)
        require(refused.returncode and build(app) == previous and sha(exe) == old_hash, "Unsafe ZIP replacement")
        require(not list(root.glob(".keefi-*")), "Failed install left an install lock or staged backup")

        with tempfile.TemporaryDirectory(prefix="KEEFI-update-") as incoming:
            download = Path(incoming) / archive.name
            shutil.copyfile(archive, download)
            old = subprocess.Popen([exe, "--profile-dir", profile], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            installer = None
            try:
                time.sleep(1.5)
                require(old.poll() is None, "Previous app exited before the upgrade")
                installer = subprocess.Popen(["/usr/bin/osascript", scripts / "restart.applescript",
                                               (scripts / "replace.sh").read_text(), str(old.pid), download,
                                               *args, "--profile-dir", profile])
                time.sleep(1)
                require(build(app) == previous and installer.poll() is None, "Installer did not wait for app exit")
                old.terminate()
                old.wait(timeout=15)
                require(installer.wait(timeout=90) == 0 and build(app) == expected, "App bundle was not updated")
                for _ in range(50):
                    active = running(exe)
                    if active:
                        break
                    time.sleep(0.1)
                requested = "--profile-dir " + str(profile)
                require(any(requested in cmd for cmd in active.values()), "New app did not reopen the same profile")
            finally:
                if old.poll() is None:
                    old.terminate()
                    old.wait(timeout=15)
                if installer and installer.poll() is None:
                    installer.terminate()
                    installer.wait(timeout=15)
                for pid in running(exe):
                    os.kill(pid, signal.SIGTERM)
        subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", app], check=True)
        subprocess.run([probe, "check", profile], check=True)
        require(stored_bytes(profile) == before, "Encrypted vault or profile identity changed")
    return dict(previous_build=previous, new_build=expected, archive_sha256=digest, relaunch_confirmed=True,
                profile_argument_preserved=True, encrypted_vault_unchanged=True, profile_marker_unchanged=True,
                synthetic_vault_reopened=True, accounts=["kiwoom", "toss"], changed_zip_refused=True)


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    for flag in ("old-bundle", "zip", "profile-probe"):
        cli.add_argument("--" + flag, required=True, type=Path)
    options = cli.parse_args()
    print(json.dumps(check(options.old_bundle, options.zip, options.profile_probe), indent=2))
