#!/usr/bin/env python3
"""Build two isolated KEEFI bundles from one Git commit and compare every entry."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import shlex
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CHANNELS = {"KEEFI": "KEEFI.app", "KEEFI_Dev": "KEEFI Dev.app"}


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def manifest(bundle):
    """Do not follow symlinks; include names, kinds, permissions and file bytes."""
    bundle = Path(bundle)
    if not bundle.is_dir() or bundle.is_symlink():
        raise ValueError("Expected a real bundle directory: " + str(bundle))
    entries = {".": {"kind": "directory", "mode": format(stat.S_IMODE(bundle.lstat().st_mode), "04o")}}
    for directory, dirs, files in os.walk(bundle, followlinks=False):
        for name in sorted(dirs + files):
            path = Path(directory) / name
            info = path.lstat()
            item = {"mode": format(stat.S_IMODE(info.st_mode), "04o")}
            if stat.S_ISLNK(info.st_mode):
                item.update(kind="symlink", target=os.readlink(path))
            elif stat.S_ISREG(info.st_mode):
                item.update(kind="file", size=info.st_size, sha256=digest(path))
            elif stat.S_ISDIR(info.st_mode):
                item["kind"] = "directory"
            else:
                raise ValueError("Unsupported bundle entry: " + str(path))
            entries[path.relative_to(bundle).as_posix()] = item
    if len(entries) == 1:
        raise ValueError("Empty bundle: " + str(bundle))
    return dict(sorted(entries.items()))


def manifest_digest(entries):
    encoded = json.dumps(entries, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def compare(left, right):
    a, b = manifest(left), manifest(right)
    differences = []
    for path in sorted(a.keys() | b.keys()):
        if a.get(path) != b.get(path):
            differences.append({"path": path, "left": a.get(path), "right": b.get(path)})
    return {"equal": not differences, "left": str(left), "right": str(right),
            "left_entries": len(a), "right_entries": len(b),
            "left_manifest_sha256": manifest_digest(a),
            "right_manifest_sha256": manifest_digest(b), "differences": differences}


def save_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def output_directory(value):
    output = Path(value).resolve()
    # Never reuse a previous run or overwrite an installed app.
    if output.exists():
        raise ValueError("Use a new output directory: " + str(output))
    if output == Path("/Applications") or Path("/Applications") in output.parents:
        raise ValueError("Output must be outside /Applications")
    output.mkdir(parents=True)
    return output


def capture(args, **kwargs):
    return subprocess.check_output([str(x) for x in args], text=True, **kwargs).strip()


def logged(args, log, env):
    with log.open("w") as stream:
        subprocess.run([str(x) for x in args], stdout=stream, stderr=subprocess.STDOUT,
                       env=env, check=True)


def reproducible_environment(epoch):
    if not str(epoch).isascii() or not str(epoch).isdigit() or not 0 < int(epoch) <= 253402300799:
        raise ValueError("SOURCE_DATE_EPOCH must be seconds in 1..253402300799")
    environment = os.environ.copy()
    for key in ("CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "QT_QPA_PLATFORM", "QT_PLUGIN_PATH",
                "QT_QPA_PLATFORM_PLUGIN_PATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH"):
        environment.pop(key, None)
    environment.update(SOURCE_DATE_EPOCH=str(epoch), QT_RCC_SOURCE_DATE_OVERRIDE=str(epoch),
                       TZ="UTC", LC_ALL="C")
    return environment


def probe_rcc(args):
    output = output_directory(args.output)
    rcc = Path(args.qt_prefix).resolve(strict=True) / "libexec/rcc"
    result = {"rcc": str(rcc), "version": capture([rcc, "--version"]), "modes": {}}
    for mode in ("unfixed", "source_date_epoch", "qt_override"):
        hashes = []
        for label, mtime in (("a", 1700000000), ("b", 1750000000)):
            directory = output / mode / label
            directory.mkdir(parents=True)
            data = directory / "probe.txt"
            data.write_text("KEEFI synthetic resource timestamp probe\n")
            os.utime(data, (mtime, mtime))
            qrc = directory / "probe.qrc"
            qrc.write_text('<RCC><qresource prefix="/probe"><file>probe.txt</file></qresource></RCC>\n')
            environment = os.environ.copy()
            environment.pop("SOURCE_DATE_EPOCH", None)
            environment.pop("QT_RCC_SOURCE_DATE_OVERRIDE", None)
            if mode == "source_date_epoch":
                environment["SOURCE_DATE_EPOCH"] = "1700000000"
            elif mode == "qt_override":
                environment["QT_RCC_SOURCE_DATE_OVERRIDE"] = "1700000000"
            binary = directory / "probe.rcc"
            logged([rcc, "--binary", "--output", binary, qrc], directory / "rcc.log", environment)
            hashes.append(digest(binary))
        result["modes"][mode] = {"a_sha256": hashes[0], "b_sha256": hashes[1], "equal": hashes[0] == hashes[1]}
    result["passed"] = (not result["modes"]["unfixed"]["equal"]
                        and result["modes"]["source_date_epoch"]["equal"]
                        and result["modes"]["qt_override"]["equal"])
    save_json(output / "receipt.json", result)
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


def export_commit(commit, destination, archive):
    subprocess.run(["git", "-C", str(ROOT), "archive", "--format=tar", "--output=" + str(archive), commit], check=True)
    destination.mkdir()
    with tarfile.open(archive) as stream:
        stream.extractall(destination, filter="data")
    archive.unlink()


def smoke(bundle, output, env):
    info = plistlib.loads((bundle / "Contents/Info.plist").read_bytes())
    executable = bundle / "Contents/MacOS" / info["CFBundleExecutable"]
    with tempfile.TemporaryDirectory(prefix="empty-profile-", dir=output) as temporary:
        report = Path(temporary) / "smoke.json"
        subprocess.run([str(executable), "--profile-dir", str(Path(temporary) / "profile"),
                        "--smoke-report", str(report)], env=env, check=True,
                       capture_output=True, timeout=30)
        result = json.loads(report.read_text())
    expected = {"read_only_build": True, "account_count": 0, "vault_exists": False,
                "vault_unlocked": False, "visible": True, "security_ready": True}
    for key, value in expected.items():
        if result.get(key) != value:
            raise ValueError("Empty-profile smoke mismatch: " + key)
    return result


def build_pair(args):
    if platform.system() != "Darwin" or platform.machine() != "x86_64":
        raise ValueError("This build procedure is verified for Intel macOS only")
    commit = capture(["git", "-C", ROOT, "rev-parse", "--verify", args.ref + "^{commit}"])
    epoch = args.epoch or capture(["git", "-C", ROOT, "show", "-s", "--format=%ct", commit])
    environment = reproducible_environment(epoch)
    qt = Path(args.qt_prefix).resolve(strict=True)
    sodium = Path(args.sodium_prefix).resolve(strict=True)
    cmake = Path(shutil.which(args.cmake) or args.cmake).resolve(strict=True)
    ninja = Path(shutil.which(args.ninja) or args.ninja).resolve(strict=True)
    output = output_directory(args.output)
    helper_source = Path(__file__).read_bytes()
    (output / "helper-executed.py").write_bytes(helper_source)
    receipt = {"format": "keefi-reproducible-build/1", "ref": args.ref, "commit": commit,
               "source_date_epoch": int(epoch), "source_state": "two independent git archive exports of one commit",
               "compiler": capture(["/usr/bin/clang++", "--version"]),
               "cmake": capture([cmake, "--version"]).splitlines()[0],
               "ninja": capture([ninja, "--version"]), "qt": capture([qt / "libexec/rcc", "--version"]),
               "macos": capture(["/usr/bin/sw_vers", "-productVersion"]),
               "sdk": capture(["/usr/bin/xcrun", "--show-sdk-version"]),
               "helper_sha256": hashlib.sha256(helper_source).hexdigest(), "runs": {}, "comparisons": {},
               "installed_apps_changed": False, "financial_accounts_used": False,
               "full_readonly_tests_run": False, "rl01_complete": False}
    try:
        for label in ("a", "b"):
            print("Preparing clean build " + label, flush=True)
            source = output / ("source-" + label)
            build = output / ("build-" + label)
            export_commit(commit, source, output / ("source-" + label + ".tar"))
            flags = shlex.join(["-ffile-prefix-map=" + str(source) + "=/keefi/source",
                                "-ffile-prefix-map=" + str(build) + "=/keefi/build"])
            configure = [cmake, "-S", source / "fincept-qt", "-B", build, "-G", "Ninja",
                         "-DCMAKE_MAKE_PROGRAM=" + str(ninja), "-DCMAKE_PREFIX_PATH=" + str(qt),
                         "-DKEEFI_SODIUM_ROOT=" + str(sodium), "-DKEEFI_G1_MODE=ON", "-DKEEFI_READ_ONLY_BETA=ON",
                         "-DKEEFI_BUILD_TESTS=OFF", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_OSX_ARCHITECTURES=x86_64",
                         "-DCMAKE_CXX_FLAGS=" + flags, "-DCMAKE_OBJCXX_FLAGS=" + flags]
            logged(configure, output / ("configure-" + label + ".log"), environment)
            logged([cmake, "--build", build, "--target", *CHANNELS, "--parallel", args.jobs],
                   output / ("build-" + label + ".log"), environment)
            print("Packaging clean build " + label, flush=True)
            runs = {}
            for target, name in CHANNELS.items():
                bundle = build / ".app-staging.noindex" / name
                logged([qt / "bin/macdeployqt", bundle, "-no-strip", "-always-overwrite", "-codesign=-"],
                       output / ("package-" + label + "-" + target + ".log"), environment)
                subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(bundle)],
                               check=True, capture_output=True)
                entries = manifest(bundle)
                save_json(output / ("manifest-" + label + "-" + target + ".json"), entries)
                runs[target] = {"bundle": str(bundle), "manifest_sha256": manifest_digest(entries),
                                "entries": len(entries), "smoke": smoke(bundle, output, environment)}
            receipt["runs"][label] = runs
        for target, name in CHANNELS.items():
            receipt["comparisons"][target] = compare(output / "build-a/.app-staging.noindex" / name,
                                                    output / "build-b/.app-staging.noindex" / name)
        receipt["pair_equal"] = all(item["equal"] for item in receipt["comparisons"].values())
        receipt["rl01_remaining"] = "Candidate must be built from the release tag and compared with its installed same-channel bundle; two clean builds alone do not complete RL-01."
    except Exception as error:
        receipt["error"] = type(error).__name__ + ": " + str(error)
        raise
    finally:
        receipt["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
        save_json(output / "receipt.json", receipt)
    print(json.dumps({"pair_equal": receipt["pair_equal"], "receipt": str(output / "receipt.json"),
                      "rl01_complete": False}, indent=2))
    return 0 if receipt["pair_equal"] else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    probe = commands.add_parser("probe-rcc", help="Check real rcc output with two different resource mtimes")
    probe.add_argument("--qt-prefix", required=True)
    probe.add_argument("--output", required=True)
    pair = commands.add_parser("build-pair", help="Build and package both channels twice in a new directory")
    pair.add_argument("--ref", required=True, help="Frozen Git commit/tag; working-tree changes are excluded")
    pair.add_argument("--qt-prefix", required=True)
    pair.add_argument("--sodium-prefix", required=True)
    pair.add_argument("--cmake", default="cmake")
    pair.add_argument("--ninja", default="ninja")
    pair.add_argument("--jobs", type=int, choices=range(1, 9), default=4)
    pair.add_argument("--epoch", help="Positive seconds since epoch; defaults to commit time")
    pair.add_argument("--output", required=True)
    comparison = commands.add_parser("compare", help="Read-only comparison of two existing bundles")
    comparison.add_argument("left")
    comparison.add_argument("right")
    comparison.add_argument("--report", required=True)
    args = parser.parse_args(argv)
    if args.command == "probe-rcc":
        return probe_rcc(args)
    if args.command == "build-pair":
        return build_pair(args)
    report = Path(args.report)
    if report.exists() or report.is_symlink():
        raise ValueError("Use a new report path: " + str(report))
    result = compare(Path(args.left), Path(args.right))
    save_json(report, result)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["equal"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(2)
