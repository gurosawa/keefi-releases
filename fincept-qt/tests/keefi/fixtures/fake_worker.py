"""G1 test fixture. Only synthetic credentials; never contacts a service.

Each deployment-pinned real worker must establish its own protections before
emitting this handshake. No secret is received through argv, env, or a file.
"""
import json
import os
import struct
import sys
import time


def protected():
    if sys.platform == "darwin":
        import resource
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        return resource.getrlimit(resource.RLIMIT_CORE) == (0, 0)
    if sys.platform == "win32":
        import ctypes
        import winreg
        wer = ctypes.WinDLL("wer")
        flags = ctypes.c_ulong()
        if wer.WerSetFlags(1) != 0:  # WER_FAULT_REPORTING_FLAG_NOHEAP
            return False
        if wer.WerGetFlags(ctypes.c_void_p(-1), ctypes.byref(flags)) != 0 or not flags.value & 1:
            return False
        path = r"SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps"
        for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
            for view in (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY):
                try:
                    with winreg.OpenKey(hive, path, 0, winreg.KEY_READ | view):
                        return False
                except FileNotFoundError:
                    pass
                except OSError:
                    return False
        return True
    return False


def send(value):
    body = json.dumps(value, separators=(",", ":")).encode("utf-8")
    sys.stdout.buffer.write(struct.pack(">I", len(body)) + body)
    sys.stdout.buffer.flush()


send({"ready": True, "protected": protected(), "version": 1})
header = sys.stdin.buffer.read(4)
if len(header) != 4:
    sys.exit(2)
length = struct.unpack(">I", header)[0]
if not 37 < length <= 65536 + 37:
    sys.exit(2)
body = sys.stdin.buffer.read(length)
if len(body) != length or sys.stdin.buffer.read(1):
    sys.exit(2)
request_id, payload = body.split(b"\n", 1)
request_id = request_id.decode("ascii")
try:
    request = json.loads(payload)
    secret = request["secret"]
    mode = request.get("mode", "normal")
except (ValueError, KeyError, TypeError):
    sys.exit(2)
assert isinstance(secret, str) and secret.startswith("KEEFI_FAKE_")
clean = all(secret not in item for item in sys.argv)
clean = clean and all(secret not in key + value for key, value in os.environ.items())
clean = clean and "KEEFI_PARENT_SECRET" not in os.environ
if "otherFd" in request:
    try:
        os.fstat(request["otherFd"])
        clean = False
    except OSError:
        pass
if mode == "hold":
    time.sleep(2)
elif mode == "timeout":
    time.sleep(30)
elif mode == "crash":
    os.abort()
elif mode == "stderr":
    sys.stderr.write(secret + "\n")
    sys.stderr.flush()
elif mode == "oversize":
    sys.stdout.buffer.write(struct.pack(">I", 1000000))
    sys.stdout.buffer.flush()
    time.sleep(1)
elif mode == "bad-id":
    request_id = "not-the-request-id"
elif mode == "echo":
    send({"requestId": request_id, "status": secret})
    sys.exit(0)
send({"requestId": request_id, "status": "ok" if clean else "rejected"})
