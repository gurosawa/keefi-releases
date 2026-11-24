"""Exercise both Windows Error Reporting policies on a disposable GitHub runner."""

from contextlib import contextmanager, ExitStack
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import uuid
import winreg
import zipfile


ROOT = Path(__file__).resolve().parents[2]
WER = r"SOFTWARE\Microsoft\Windows\Windows Error Reporting"
VIEWS = (("HKLM", winreg.HKEY_LOCAL_MACHINE), ("HKCU", winreg.HKEY_CURRENT_USER))
RENAME = ctypes.WinDLL("advapi32").RegRenameKey
RENAME.argtypes = (wintypes.HKEY, wintypes.LPCWSTR, wintypes.LPCWSTR)
RENAME.restype = wintypes.LONG


def rename(parent, old, new):
    code = RENAME(int(parent), old, new)
    if code:
        raise ctypes.WinError(code)


@contextmanager
def without_local_dumps():
    """Rename existing subtrees in place to preserve their full ACLs, values and children."""
    backup = "KEEFI-CI-" + uuid.uuid4().hex
    with ExitStack() as restore:
        for label, hive in VIEWS:
            for bits, view in ((64, winreg.KEY_WOW64_64KEY), (32, winreg.KEY_WOW64_32KEY)):
                try:
                    policy = winreg.OpenKey(hive, WER + r"\LocalDumps", 0, winreg.KEY_READ | view)
                except FileNotFoundError:
                    continue
                with policy:
                    child_count, value_count, _ = winreg.QueryInfoKey(policy)
                    names = {winreg.EnumValue(policy, i)[0].lower() for i in range(value_count)}
                    known = sorted(names.intersection({"dumpfolder", "dumptype", "dumpcount", "customdumpflags"}))
                    matches = any(winreg.EnumKey(policy, i).lower() == "keefi.exe" for i in range(child_count))
                    print(f"{label}/{bits} LocalDumps: {child_count} application keys; "
                          f"KEEFI-specific={matches}; global setting names={known}", flush=True)
                parent = restore.enter_context(winreg.OpenKey(hive, WER, 0, winreg.KEY_WRITE | winreg.KEY_READ | view))
                rename(parent, "LocalDumps", backup)
                restore.callback(rename, parent, backup, "LocalDumps")
        yield


@contextmanager
def dump_capable_policy():
    for _, hive in VIEWS:
        for view in (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY):
            try:
                key = winreg.OpenKey(hive, WER + r"\LocalDumps", 0, winreg.KEY_READ | view)
            except FileNotFoundError:
                continue
            key.Close()
            yield
            return
    # Keep the refusal assertion when a future runner image comes without this policy.
    with ExitStack() as cleanup:
        parts = (WER, "LocalDumps", "KEEFI.exe")
        parent = winreg.HKEY_CURRENT_USER
        for part in parts:
            try:
                key = winreg.OpenKey(parent, part, 0, winreg.KEY_READ | winreg.KEY_WRITE)
            except FileNotFoundError:
                key = winreg.CreateKeyEx(parent, part)
                cleanup.callback(winreg.DeleteKey, parent, part)
            parent = cleanup.enter_context(key)
        winreg.SetValueEx(parent, "DumpType", 0, winreg.REG_DWORD, 1)
        yield


def verify():
    if not (os.environ.get("GITHUB_ACTIONS") == "true" and os.environ.get("RUNNER_ENVIRONMENT") == "github-hosted"
            and os.environ.get("RUNNER_OS") == "Windows"):
        raise RuntimeError("This registry fixture is reserved for disposable GitHub-hosted Windows runners")
    work = Path(os.environ["RUNNER_TEMP"]) / "keefi"
    build_script = ROOT / "scripts/keefi/build-windows-candidate.ps1"
    with without_local_dumps():
        subprocess.run(["pwsh", "-NoProfile", "-NonInteractive", "-File", build_script, "-WorkDir", work], check=True)

    dist = work / "dist"
    archive, = dist.glob("KEEFI-*-windows-x64.zip")
    receipt, = dist.glob("KEEFI-*-windows-x64-verification.json")
    data = json.loads(receipt.read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory(dir=work) as tmp:
        test = Path(tmp)
        with zipfile.ZipFile(archive) as package:
            package.extractall(test)
        app = test / "KEEFI"
        with dump_capable_policy():
            report = test / "dump-blocked.json"
            env = {k: v for k, v in os.environ.items() if not k.upper().startswith(("QT_", "QML", "KEEFI_", "FINIX_"))}
            system = Path(os.environ["SystemRoot"])
            env.update(PATH=os.pathsep.join(map(str, (system / "System32", system))), QT_QPA_PLATFORM="windows")
            result = subprocess.run([app / "KEEFI.exe", "--profile-dir", test / "blocked-profile",
                                     "--smoke-report", report], cwd=app, env=env, timeout=90)
            status = json.loads(report.read_text(encoding="utf-8"))
            expected = {"startup_phase": "vault_gate", "security_ready": False, "process_hardened": False,
                        "security_status": "local_dumps_enabled_or_unreadable", "vault_unlocked": False,
                        "account_count": 0, "windows_schannel_ready": True}
            if result.returncode != 6 or any(status.get(k) != v for k, v in expected.items()):
                raise RuntimeError(f"Unexpected dump-enabled startup status: {status}")
            print("Confirmed that the extracted KEEFI.exe refused private access under LocalDumps", flush=True)
        with without_local_dumps():
            smoke = runpy.run_path(str(ROOT / "scripts/keefi/build-readonly-windows.py"))["smoke_extracted"]
            reports, failed = smoke(app, test, data["version"])
            if failed:
                raise RuntimeError(f"Clean runner startup failed: {failed}")
    with without_local_dumps():
        run_update = runpy.run_path(str(ROOT / ".github/scripts/verify-windows-update.py"))["verify"]
        data["update_install"] = run_update(work, archive, data)
        print("Verified Windows update, tamper refusal, rollback and retained encrypted accounts", flush=True)
    data["hosted_wer_policy_check"] = {"dump_enabled_refusal": expected, "normal_startups": reports,
                                       "runner_policy_restored": True}
    receipt.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    verify()
