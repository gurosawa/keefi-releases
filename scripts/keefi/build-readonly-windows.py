"""Build, test and stage a portable x64 Windows KEEFI release with the Qt MinGW kit."""

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[2]
SUITES = ("kiwoom", "kiwoom_trading", "retired_connections", "connection_form", "local_window", "profile_upgrade")
FORM_CASES = ("kiwoomFilePairIsValidatedBeforeUpdatingEitherField",
              "suspendedBrokersRejectNewAndExistingCredentials", "stockChartChooserUsesLiveBrokersAndReturnsToChart")
WINDOW_CASES = ("brokerConnectionReturnsToOrigin", "suspendedBrokersKeepOnlyRemovalAction")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def file_version(path):
    """Read VS_FIXEDFILEINFO, independent of the Windows UI language."""
    api = ctypes.windll.version
    path_string = ctypes.c_wchar_p(str(path))
    size = api.GetFileVersionInfoSizeW(path_string, None)
    require(size > 0, "KEEFI.exe is missing its Windows version resource")
    buffer = ctypes.create_string_buffer(size)
    require(api.GetFileVersionInfoW(path_string, 0, size, buffer), "Invalid Windows version resource")
    value = ctypes.c_void_p()
    length = ctypes.c_uint()
    require(api.VerQueryValueW(buffer, ctypes.c_wchar_p("\\"), ctypes.byref(value), ctypes.byref(length)),
            "Missing VS_FIXEDFILEINFO")
    require(length.value >= 24, "Truncated Windows version resource")
    signature, _, major_minor, patch_build, product_ms, product_ls = struct.unpack(
        "<6I", ctypes.string_at(value, 24))
    require(signature == 0xFEEF04BD and (major_minor, patch_build) == (product_ms, product_ls),
            "Windows file and product versions differ")
    return (f"{major_minor >> 16}.{major_minor & 0xffff}.{patch_build >> 16}", patch_build & 0xffff)


def deploy_dependencies(app, objdump, sdk_bins, env):
    """windeployqt handles Qt plugins. Resolve non-Qt imports such as zlib from the same SDKs."""
    system = Path(os.environ["SystemRoot"]) / "System32"
    root_dlls = {p.name.lower() for p in app.glob("*.dll")}
    binaries = [app / "KEEFI.exe", *app.rglob("*.dll")]
    while binaries:
        binary = binaries.pop()
        info = subprocess.check_output([objdump, "-p", binary], env=env).decode("utf-8", "replace")
        require("file format pei-x86-64" in info, f"Expected a Windows x64 binary: {binary.name}")
        for imported in re.findall(r"^\s*DLL Name:\s*(\S+)\s*$", info, flags=re.MULTILINE):
            dll = imported.lower()
            require(Path(dll).name == dll and "sodium" not in dll,
                    f"Unexpected dependency of {binary.name}: {imported} (libsodium must be static)")
            if dll in root_dlls or dll.startswith(("api-ms-win-", "ext-ms-win-")):
                continue
            # The MinGW runtimes must travel with the app even if the build host has an extra system copy.
            if not dll.startswith(("libgcc_", "libstdc++", "libwinpthread", "qt6", "zlib")) and (system / dll).is_file():
                continue
            source = next((d / imported for d in sdk_bins if (d / imported).is_file()), None)
            require(source is not None, f"Cannot deploy {imported} imported by {binary.name}")
            dest = app / imported
            shutil.copy2(source, dest)
            root_dlls.add(dll)
            binaries.append(dest)


def run_tests(build, env):
    selected = {"connection_form": FORM_CASES, "local_window": WINDOW_CASES,
                "profile_upgrade": ("emptyStateCreatesOnlyTheNewIdentity",)}
    results = {}
    for suite in SUITES:
        # QTest's Windows GUI backend exercises the native platform through the same public entry points.
        exe = build / "tests" / "keefi" / f"tst_keefi_{suite}.exe"
        log = build / f"packaging-{suite}.txt"
        log.unlink(missing_ok=True)
        result = subprocess.run([exe, *selected.get(suite, ()), "-o", f"{log},txt"], env=env, timeout=480)
        output = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
        totals = re.search(r"Totals:\s*(\d+) passed,\s*(\d+) failed,\s*(\d+) skipped,\s*(\d+) blacklisted", output)
        if not (result.returncode == 0 and totals and int(totals[1]) > 2 and totals.groups()[1:] == ("0", "0", "0")):
            for line in output.splitlines():
                if line.lstrip().startswith(("FAIL!", "QFATAL", "SKIP", "XPASS", "Loc:", "Totals:")):
                    print(line, flush=True)
            raise RuntimeError(f"Native {suite} tests exited {result.returncode}; see {log}")
        print(f"{suite}: {totals[0]}", flush=True)
        results[suite] = int(totals[1])
    return results


def isolated_tests(build, env, plugins):
    # The vault checks file ACLs. Give synthetic QTemporaryDir children a real
    # private owner-only directory without changing the tester's profile or TEMP.
    sid_output = subprocess.check_output(["whoami.exe", "/user", "/fo", "csv", "/nh"])
    sid = re.search(rb"\bS-1-\d+(?:-\d+)+\b", sid_output)
    require(sid, "Cannot identify the current Windows user for test directory permissions")
    with tempfile.TemporaryDirectory(prefix="keefi-test-") as temp:
        subprocess.run(["icacls.exe", temp, "/grant:r", f"*{sid[0].decode('ascii')}:(OI)(CI)F",
                        "/inheritance:r"], check=True)
        return run_tests(build, {**env, "QT_QPA_PLATFORM": "windows", "QT_PLUGIN_PATH": str(plugins),
                                 "TEMP": temp, "TMP": temp})


def bundle_notices(app, qt, compiler_bin, zlib):
    for name in ("LICENSE", "NOTICE"):
        shutil.copy2(ROOT / name, app)
    notices = app / "licenses"
    shutil.copytree(ROOT / "docs/keefi/licenses", notices)
    shutil.copy2(ROOT / "docs/keefi/THIRD-PARTY-NOTICES.md", app)
    # These notices come from the same Windows kits as the deployed runtime.
    for component in ("gcc", "mingw-w64", "winpthreads"):
        shutil.copytree(compiler_bin.parent / "licenses" / component, notices / component)
    for module in ("qtbase", "qtwebsockets"):
        for suffix in (".spdx", ".source.spdx"):
            shutil.copy2(qt / "sbom" / (module + "-6.8.3" + suffix), notices)
    shutil.copy2(zlib / "include/zlib.h", notices / "zlib.h")


def smoke_extracted(app, work, version):
    # Keep the desktop user's environment and profiles untouched. SDK paths must not rescue missing DLLs.
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith(("QT_", "QML", "KEEFI_", "FINIX_"))}
    system = Path(os.environ["SystemRoot"])
    env.update(PATH=os.pathsep.join(map(str, (system / "System32", system))), QT_QPA_PLATFORM="windows")
    reports = []
    for attempt in ("first", "reopen"):
        report = work / f"{attempt}.json"
        result = subprocess.run([app / "KEEFI.exe", "--profile-dir", work / "profile", "--smoke-report", report],
                                cwd=app, env=env, timeout=90)
        require(report.is_file(), f"{attempt} startup exited {result.returncode} before producing a smoke report")
        data = json.loads(report.read_text(encoding="utf-8"))
        expected = {"application": "KEEFI", "application_version": version, "build_channel": "production",
                    "read_only_build": True, "security_ready": True, "visible": True, "startup_vault_gate_visible": True,
                    "vault_unlocked": False, "vault_exists": False, "account_count": 0, "windows_schannel_ready": True}
        require(result.returncode == 0 and all(data.get(k) == v for k, v in expected.items()),
                f"Unexpected packaged {attempt} startup (exit {result.returncode}): {data}")
        reports.append(data)
    return reports


def build_package(args):
    require(os.name == "nt", "Run on x64 Windows with the Qt 6.8.3 MinGW kit")
    build, output = args.build_dir.resolve(), args.output_dir.resolve()
    qt = args.qt_prefix.resolve()
    compiler = args.compiler_bin.resolve()
    bins = [qt / "bin", compiler, args.zlib_root.resolve() / "bin"]
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join([*map(str, bins), env["PATH"]])
    for name in ("g++.exe", "gcc.exe", "objdump.exe"):
        require((compiler / name).is_file(), f"Missing MinGW tool {compiler / name}")
    env.update(CC=str(compiler / "gcc.exe"), CXX=str(compiler / "g++.exe"))
    cmake = shutil.which(args.cmake, path=env["PATH"])
    ninja = shutil.which(args.ninja, path=env["PATH"])
    require(cmake and ninja, "CMake 3.27+ and Ninja must be available")
    config = [cmake, "-S", ROOT / "fincept-qt", "-B", build, "-G", "Ninja",
              "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_MAKE_PROGRAM={ninja}",
              f"-DCMAKE_PREFIX_PATH={qt}", f"-DKEEFI_SODIUM_ROOT={args.sodium_root.resolve()}",
              f"-DZLIB_ROOT={args.zlib_root.resolve()}", "-DZLIB_USE_STATIC_LIBS=ON",
              "-DKEEFI_G1_MODE=ON", "-DKEEFI_BUILD_TESTS=ON", "-DKEEFI_READ_ONLY_BETA=ON"]
    if args.sodium_library:
        config.append(f"-DKEEFI_SODIUM_LIBRARY={args.sodium_library.resolve(strict=True)}")
    if args.zlib_library:
        config.append(f"-DZLIB_LIBRARY_RELEASE={args.zlib_library.resolve(strict=True)}")
    subprocess.run(config, env=env, check=True)
    targets = ["KEEFI"]
    if not args.stage_only:
        targets.extend(f"tst_keefi_{suite}" for suite in SUITES)
    subprocess.run([cmake, "--build", build, "--parallel", str(args.jobs), "--target", *targets], env=env, check=True)
    tests = {} if args.stage_only else isolated_tests(build, env, qt / "plugins")
    binary = build / "KEEFI.exe"
    version, revision = file_version(binary)
    stem = f"KEEFI-{version}-{revision}-windows-x64"
    if args.stage_only:
        stem += "-unverified"
        output /= "unverified"
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="keefi-package-", dir=output) as tmp:
        work = Path(tmp)
        app = work / "KEEFI"
        app.mkdir()
        shutil.copy2(binary, app)
        subprocess.run([qt / "bin/windeployqt.exe", "--release", "--no-translations", "--compiler-runtime",
                        app / binary.name], env=env, check=True)
        # Keep all plugins beside the portable binary, including the built-in Windows TLS backend.
        (app / "qt.conf").write_text("[Paths]\nPrefix=.\nPlugins=.\n", encoding="ascii")
        required = ("platforms/qwindows.dll", "tls/qschannelbackend.dll", "Qt6Network.dll",
                    "Qt6WebSockets.dll", "Qt6Core.dll", "Qt6Widgets.dll", "Qt6Gui.dll")
        for relative in required:
            require((app / relative).is_file(), f"Missing deployed runtime {relative}")
        deploy_dependencies(app, compiler / "objdump.exe", bins, env)
        bundle_notices(app, qt, compiler, args.zlib_root)
        archive = work / f"{stem}.zip"
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as z:
            for path in sorted(app.rglob("*")):
                if path.is_file():
                    z.write(path, path.relative_to(work))
        reports = []
        if not args.stage_only:
            with zipfile.ZipFile(archive) as z:
                z.extractall(work / "unpacked")
            reports = smoke_extracted(work / "unpacked/KEEFI", work, version)
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        checksum = work / (archive.name + ".sha256")
        checksum.write_text(f"{digest}  {archive.name}\n", encoding="ascii")
        receipt = work / f"{stem}-verification.json"
        receipt.write_text(json.dumps({"verified": not args.stage_only, "version": version,
                                       "build": revision, "architecture": "x64",
                                       "archive": archive.name, "sha256": digest, "synthetic_tests": tests,
                                       "packaged_startups": reports}, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        for deliverable in (archive, checksum, receipt):
            os.replace(deliverable, output / deliverable.name)
    print(output / archive.name)
    print(f"SHA-256 {digest}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("qt-prefix", "compiler-bin", "sodium-root", "zlib-root", "build-dir", "output-dir"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument("--sodium-library", type=Path, help="Explicit static libsodium archive")
    parser.add_argument("--zlib-library", type=Path, help="Explicit zlib release archive")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ninja", default="ninja")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--stage-only", action="store_true",
                        help="Create an explicitly unverified diagnostic archive without running tests or the app")
    build_package(parser.parse_args())
