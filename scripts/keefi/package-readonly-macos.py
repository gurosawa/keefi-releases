"""Stage the Intel production bundle, verify its local runtime and produce its update ZIP."""

import argparse
import hashlib
import json
from pathlib import Path
import os
import plistlib
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FRAMEWORKS = ("QtCore", "QtDBus", "QtGui", "QtNetwork", "QtWebSockets", "QtWidgets")
PLUGINS = (
    "platforms/libqcocoa.dylib", "platforms/libqoffscreen.dylib", "styles/libqmacstyle.dylib",
    "imageformats/libqgif.dylib", "imageformats/libqico.dylib", "imageformats/libqjpeg.dylib",
    "tls/libqsecuretransportbackend.dylib", "tls/libqcertonlybackend.dylib", "tls/libqopensslbackend.dylib",
    "networkinformation/libqscnetworkreachability.dylib",
)


def run(*args, **kwargs):
    return subprocess.run(list(map(str, args)), check=True, text=True, **kwargs)


def output(*args):
    return run(*args, stdout=subprocess.PIPE).stdout.strip()


def require(ok, message):
    if not ok:
        raise ValueError(message)


def stage_binary(source, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    archs = output("/usr/bin/lipo", "-archs", source).split()
    require("x86_64" in archs, f"Intel slice missing in {source.name}")
    if len(archs) > 1:
        run("/usr/bin/lipo", source, "-thin", "x86_64", "-output", dest)
    else:
        shutil.copyfile(source, dest)
    dest.chmod(0o755)


def localize(path, identity):
    changes = []
    deps = output("/usr/bin/otool", "-L", path).splitlines()[1:]
    if identity:
        changes.extend(("-id", identity))
        deps = deps[1:]
    for entry in deps:
        name = entry.strip().split(" (compatibility version", 1)[0]
        if name.startswith(("/System/Library/", "/usr/lib/")):
            continue
        framework = re.search(r"(Qt\w+)\.framework/Versions/A/(Qt\w+)$", name)
        require(framework and framework[1] == framework[2] and framework[1] in FRAMEWORKS,
                f"Unbundled import in {path.name}: {name}")
        dest = "@executable_path/../Frameworks/" + framework[0]
        if name != dest:
            changes.extend(("-change", name, dest))
    for value in re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset \d+\)", output("/usr/bin/otool", "-l", path)):
        if value != "@executable_path/../Frameworks":
            changes.extend(("-delete_rpath", value))
    if changes:
        run("/usr/bin/install_name_tool", *changes, path)


def sign(path):
    run("/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", path)


def package(raw, qt, patch_build, dest):
    raw, qt, patch_build = (path.resolve(strict=True) for path in (raw, qt, patch_build))
    info = plistlib.loads((raw / "Contents/Info.plist").read_bytes())
    require(info["CFBundleIdentifier"] == "xyz.beamgrove.keefi.local" and info["CFBundleExecutable"] == "KEEFI",
            "A production KEEFI bundle is required")
    dest.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=dest) as tmp:
        work = Path(tmp)
        app = work / "KEEFI.app"
        contents = app / "Contents"
        contents.mkdir(parents=True)
        shutil.copyfile(raw / "Contents/Info.plist", contents / "Info.plist")
        binary = contents / "MacOS/KEEFI"
        stage_binary(raw / "Contents/MacOS/KEEFI", binary)
        localize(binary, None)
        for name in FRAMEWORKS:
            framework = contents / "Frameworks" / (name + ".framework")
            sdk_framework = qt / "lib" / framework.name / "Versions/A"
            target = framework / "Versions/A"
            target.mkdir(parents=True)
            (framework / "Versions/Current").symlink_to("A")
            (framework / name).symlink_to("Versions/Current/" + name)
            (framework / "Resources").symlink_to("Versions/Current/Resources")
            shutil.copytree(sdk_framework / "Resources", target / "Resources", ignore=shutil.ignore_patterns("*.prl"))
            src = patch_build / "lib" / framework.name / "Versions/A" / name if name == "QtGui" else sdk_framework / name
            stage_binary(src, target / name)
            localize(target / name, f"@rpath/{framework.name}/Versions/A/{name}")
            sign(framework)
        for name in PLUGINS:
            dest_plugin = contents / "PlugIns" / name
            src = (patch_build if name.endswith("/libqcocoa.dylib") else qt) / "plugins" / name
            stage_binary(src, dest_plugin)
            localize(dest_plugin, None)
            sign(dest_plugin)
        resources = contents / "Resources"
        resources.mkdir()
        (resources / "qt.conf").write_text("[Paths]\nPlugins = PlugIns\n", encoding="utf-8")
        notices = resources / "licenses"
        shutil.copytree(ROOT / "docs/keefi/licenses", notices)
        for module in ("qtbase", "qtwebsockets"):
            for suffix in (".spdx", ".source.spdx"):
                name = f"{module}-6.8.3{suffix}"
                shutil.copyfile(qt / "sbom" / name, notices / name)
        for src in ("LICENSE", "NOTICE", "docs/keefi/THIRD-PARTY-NOTICES.md",
                    "docs/keefi/patches/qtbase-6.8.3-macos-accessibility.patch"):
            shutil.copyfile(ROOT / src, resources / Path(src).name)
        sign(app)
        run("/usr/bin/codesign", "--verify", "--deep", "--strict", app)
        version, build = info["CFBundleShortVersionString"], info["CFBundleVersion"]
        exe_name = f"KEEFI-{version}-{build}-macos-x86_64.zip"
        archive = work / exe_name
        run("/usr/bin/ditto", "-c", "-k", "--keepParent", "--norsrc", app, archive)
        extracted = work / "expanded"
        run("/usr/bin/ditto", "-x", "-k", archive, extracted)
        launch = extracted / "KEEFI.app"
        run("/usr/bin/codesign", "--verify", "--deep", "--strict", launch)
        env = {k: v for k, v in os.environ.items() if not k.startswith(("QT_", "DYLD_", "QML", "KEEFI_", "FINIX_"))}
        report = work / "smoke.json"
        run(launch / "Contents/MacOS/KEEFI", "--profile-dir", work / "clean-profile", "--smoke-report", report,
            env=dict(env, QT_QPA_PLATFORM="cocoa"), timeout=60)
        smoke = json.loads(report.read_text())
        require(smoke["application_version"] == version and smoke["security_ready"] and smoke["visible"]
                and smoke["startup_vault_gate_visible"] and smoke["build_channel"] == "production", "Startup failed")
        sha = hashlib.sha256(archive.read_bytes()).hexdigest()
        archive.replace(dest / exe_name)
        (dest / (exe_name + ".sha256")).write_text(f"{sha}  {exe_name}\n")
        return sha


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("raw-bundle", "qt-prefix", "patched-qt-build", "output-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    print(package(args.raw_bundle, args.qt_prefix, args.patched_qt_build, args.output_dir))
