"""Exercise the installed Windows updater against a disposable 111 application and encrypted profile."""
import ctypes
from ctypes import wintypes
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import zipfile

BASE = "KEEFI-0.18.0-111-windows-x64.zip"
BASE_SHA = "0afc6313fa46c7e2821e94a083ade861f3da045b4c15e776f84cdaa3c76155c9"
BASE_URL = "https://github.com/gurosawa/keefi-releases/releases/download/v0.18.0-rc.111/" + BASE
kernel = ctypes.WinDLL("kernel32", use_last_error=True)
user = ctypes.WinDLL("user32", use_last_error=True)
kernel.WaitForSingleObject.argtypes = (wintypes.HANDLE,wintypes.DWORD)
kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
kernel.GetExitCodeProcess.argtypes = (wintypes.HANDLE,ctypes.POINTER(wintypes.DWORD))
kernel.OpenProcess.argtypes = (wintypes.DWORD,wintypes.BOOL,wintypes.DWORD)
kernel.OpenProcess.restype = wintypes.HANDLE
kernel.QueryFullProcessImageNameW.argtypes = (wintypes.HANDLE,wintypes.DWORD,wintypes.LPWSTR,ctypes.POINTER(wintypes.DWORD))
kernel.CreateFileW.argtypes = (wintypes.LPCWSTR,wintypes.DWORD,wintypes.DWORD,ctypes.c_void_p,wintypes.DWORD,wintypes.DWORD,wintypes.HANDLE)
kernel.CreateFileW.restype = wintypes.HANDLE
window_callback = ctypes.WINFUNCTYPE(wintypes.BOOL,wintypes.HWND,wintypes.LPARAM)
user.EnumWindows.argtypes = (window_callback,wintypes.LPARAM)
user.GetWindowThreadProcessId.argtypes = (wintypes.HWND,ctypes.POINTER(wintypes.DWORD))
user.IsWindowVisible.argtypes = (wintypes.HWND,)
user.GetWindowTextW.argtypes = (wintypes.HWND,wintypes.LPWSTR,ctypes.c_int)
user.PostMessageW.argtypes = (wintypes.HWND,wintypes.UINT,wintypes.WPARAM,wintypes.LPARAM)
user.PostThreadMessageW.argtypes = (wintypes.DWORD,wintypes.UINT,wintypes.WPARAM,wintypes.LPARAM)


def digest(file):
    return hashlib.sha256(file.read_bytes()).hexdigest()


def window(exe, wanted_pid=None, exclude=0):
    result = []
    def visit(hwnd, _):
        if not user.IsWindowVisible(hwnd): return True
        pid = wintypes.DWORD()
        tid = user.GetWindowThreadProcessId(hwnd,ctypes.byref(pid))
        if pid.value == exclude or (wanted_pid is not None and pid.value != wanted_pid): return True
        handle = kernel.OpenProcess(0x1000,False,pid.value)
        if not handle: return True
        try:
            name = ctypes.create_unicode_buffer(32768)
            size = wintypes.DWORD(len(name))
            if not kernel.QueryFullProcessImageNameW(handle,0,name,ctypes.byref(size)): return True
            title = ctypes.create_unicode_buffer(512)
            user.GetWindowTextW(hwnd,title,len(title))
            if (exe is None or Path(name.value)==exe) and "KEEFI" in title.value:
                result.append((hwnd,tid,pid.value))
                return False
        finally:
            kernel.CloseHandle(handle)
        return True
    user.EnumWindows(window_callback(visit),0)
    return result[0] if result else None


def await_window(exe, wanted_pid=None, exclude=0):
    end=time.monotonic()+35
    while time.monotonic()<end:
        item=window(exe,wanted_pid,exclude)
        if item: return item
        time.sleep(.12)
    raise RuntimeError("No visible window for the installed executable")


def stop_window(info):
    hwnd,tid,pid=info
    handle=kernel.OpenProcess(0x100000,False,pid)
    if not handle: return
    try:
        user.PostMessageW(hwnd,0x0010,0,0)  # WM_CLOSE
        if kernel.WaitForSingleObject(handle,5000)!=0:
            # Background-enabled 111 can keep a tray instance on WM_CLOSE.
            # Quit its GUI event loop; Qt still emits aboutToQuit.
            user.PostThreadMessageW(tid,0x0012,0,0)  # WM_QUIT
            assert kernel.WaitForSingleObject(handle,10000)==0
    finally:
        kernel.CloseHandle(handle)


def commandline_uses_profile(pid, profile):
    expression=f"(Get-CimInstance Win32_Process -Filter 'ProcessId = {pid}').CommandLine"
    command=subprocess.check_output(["powershell.exe","-NoProfile","-NonInteractive","-Command",expression])
    return b"--profile-dir" in command and str(profile).lower() in command.decode("utf-8","replace").lower()


class DetachedUpdater:
    def __init__(self,pid):
        self.pid=pid
        self.handle=kernel.OpenProcess(0x100000|0x1000,False,pid)
        assert self.handle

    def wait(self,seconds):
        try:
            assert kernel.WaitForSingleObject(self.handle,int(seconds*1000))==0
            code=wintypes.DWORD()
            assert kernel.GetExitCodeProcess(self.handle,ctypes.byref(code))
            return code.value
        finally:
            kernel.CloseHandle(self.handle)


def helper_process(driver, env, old, exe, archive, sha, version, build, profile, mode):
    # The detached installer can keep inherited output handles open while it
    # waits for the old app. Wait for the driver itself, then read its PID.
    with tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as errors:
        attempt = subprocess.run(
            [driver, str(old.pid), exe, archive, sha, version, str(build), profile],
            env=env, stdin=subprocess.DEVNULL, stdout=output, stderr=errors, timeout=25,
        )
        if mode == "tamper":
            assert attempt.returncode == 1 and old.poll() is None
            return None
        if attempt.returncode != 0:
            errors.seek(0)
            raise RuntimeError(f"Production handoff failed during {mode}: {errors.read()!r}")
        output.seek(0)
        return DetachedUpdater(int(output.readline().strip()))


def verify(work, archive, data):
    with tempfile.TemporaryDirectory(prefix="signed-update-",dir=work) as temporary:
        temp=Path(temporary)
        base=temp/BASE
        subprocess.run(["curl.exe","--fail","--silent","--show-error","--location","--retry","3","--output",base,BASE_URL],
                       check=True,timeout=180)
        assert digest(base)==BASE_SHA
        with zipfile.ZipFile(base) as previous: previous.extractall(temp/"old")
        with zipfile.ZipFile(archive) as candidate:
            helper=temp/"KEEFIUpdate.exe"
            helper.write_bytes(candidate.read("KEEFI/KEEFIUpdate.exe"))
            new_exe_hash=hashlib.sha256(candidate.read("KEEFI/KEEFI.exe")).hexdigest()
        app=temp/"old"/"KEEFI"; exe=app/"KEEFI.exe"
        old_exe_hash=digest(exe)
        assert new_exe_hash!=old_exe_hash
        # This first updater release has to be installed manually. Supply only
        # the new bootstrap helper/inventory beside the genuine 111 executable.
        # No broker/profile/test executable is copied into the candidate ZIP.
        shutil.copy2(helper,app/helper.name)
        inventory=app/"keefi-package-files.txt"
        names=sorted([p.relative_to(app).as_posix() for p in app.rglob("*") if p.is_file()]+[inventory.name])
        inventory.write_text("\n".join(names)+"\n",encoding="ascii")
        sentinel=app/"tester note.txt"; sentinel.write_bytes(b"portable-user-file")
        sdk=work/"sdk"
        test_env={**os.environ,"PATH":os.pathsep.join(map(str,(sdk/"bin",sdk/"Tools/mingw1310_64/bin",os.environ["PATH"])))}
        profile=temp/"test profile"
        probe=work/"build/tests/keefi/keefi_update_profile_probe.exe"
        driver=work/"build/tests/keefi/keefi_update_handoff_probe.exe"
        subprocess.run([probe,"seed",profile],env=test_env,check=True)
        vault=profile/"credentials.keefi-vault"; marker=profile/".keefi-profile.json"
        before=(digest(vault),digest(marker))
        tampered=temp/"tampered.zip"
        shutil.copy2(archive,tampered)
        with tampered.open("ab") as f: f.write(b"tamper")
        system=Path(os.environ["SystemRoot"])
        env={k:v for k,v in os.environ.items() if not k.upper().startswith(("QT_","QML","KEEFI_","FINIX_"))}
        env.update(PATH=os.pathsep.join(map(str,(system/"System32",system))),QT_QPA_PLATFORM="windows")
        old=subprocess.Popen([exe,"--profile-dir",profile],cwd=temp,env=env)
        try:
            original=await_window(exe,old.pid)
            print("Checking tampered archive against a running 111 application",flush=True)
            helper_process(driver,test_env,old,exe,tampered,data["sha256"],data["version"],data["build"],profile,"tamper")
            assert digest(exe)==old_exe_hash and before==(digest(vault),digest(marker))
            # Refuse to overwrite a busy existing package component after some
            # preceding files have been replaced, then relaunch the old app.
            busy=kernel.CreateFileW(str(app/"Qt6Widgets.dll"),0x80000000,1,None,3,0,None)
            assert busy and busy!=wintypes.HANDLE(-1).value
            try:
                print("Checking replacement rollback with a locked package DLL",flush=True)
                fail=helper_process(driver,test_env,old,exe,archive,data["sha256"],data["version"],data["build"],profile,"rollback")
                stop_window(original)
                restored=await_window(exe,exclude=old.pid)
                notice=await_window(None,fail.pid);user.PostMessageW(notice[0],0x0010,0,0)
                assert fail.wait(12)==19
                assert digest(exe)==old_exe_hash and sentinel.read_bytes()==b"portable-user-file"
            finally:
                kernel.CloseHandle(busy)
            stop_window(restored)
            subprocess.run([probe,"check",profile],env=test_env,check=True)
            old=subprocess.Popen([exe,"--profile-dir",profile],cwd=temp,env=env)
            original=await_window(exe,old.pid)
            print("Checking actual replacement and relaunch into the seeded profile",flush=True)
            good=helper_process(driver,test_env,old,exe,archive,data["sha256"],data["version"],data["build"],profile,"install")
            stop_window(original)
            assert good.wait(60)==0
            updated=await_window(exe,exclude=old.pid)
            assert digest(exe)==new_exe_hash and commandline_uses_profile(updated[2],profile)
            stop_window(updated)
            subprocess.run([probe,"check",profile],env=test_env,check=True)
            assert before==(digest(vault),digest(marker)) and sentinel.read_bytes()==b"portable-user-file"
        finally:
            if old.poll() is None: old.terminate();old.wait(12)
            leftover=window(exe)
            if leftover: stop_window(leftover)
    return {"from":"0.18.0 (111) with test bootstrap","to":data["version"],"build":data["build"],
            "tamper_refused":True,"locked_component_rollback":True,"updated_executable_relaunched":True,
            "same_profile_argument":True,"same_vault_and_marker_sha256":True,"retained_unmanaged_file":True,
            "password_and_credentials_verified":["kiwoom","toss"]}
