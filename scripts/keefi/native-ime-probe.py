"""Opt-in physical-key probe for the fake-password Qt test. Restores the input source."""
import ctypes as C
import subprocess, sys, time
cf=C.CDLL('/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation')
hi=C.CDLL('/System/Library/Frameworks/Carbon.framework/Carbon')
P=C.c_void_p
cf.CFStringCreateWithCString.argtypes=[P,C.c_char_p,C.c_uint32];cf.CFStringCreateWithCString.restype=P
cf.CFStringGetCString.argtypes=[P,P,C.c_long,C.c_uint32];cf.CFStringGetCString.restype=C.c_bool
cf.CFArrayGetCount.argtypes=[P];cf.CFArrayGetCount.restype=C.c_long
cf.CFArrayGetValueAtIndex.argtypes=[P,C.c_long];cf.CFArrayGetValueAtIndex.restype=P
cf.CFRelease.argtypes=[P]
hi.TISCopyCurrentKeyboardInputSource.restype=P
hi.TISCreateInputSourceList.argtypes=[P,C.c_bool];hi.TISCreateInputSourceList.restype=P
hi.TISGetInputSourceProperty.argtypes=[P,P];hi.TISGetInputSourceProperty.restype=P
hi.TISSelectInputSource.argtypes=[P];hi.TISSelectInputSource.restype=C.c_int32
key=P.in_dll(hi,'kTISPropertyInputSourceID')
def ident(source):
    value=hi.TISGetInputSourceProperty(source,key)
    buf=C.create_string_buffer(512)
    cf.CFStringGetCString(value,buf,512,0x08000100)
    return buf.value.decode()
previous=hi.TISCopyCurrentKeyboardInputSource();sources=hi.TISCreateInputSourceList(None,False)
korean=None
for i in range(cf.CFArrayGetCount(sources)):
    src=cf.CFArrayGetValueAtIndex(sources,i)
    if ident(src)=='com.apple.inputmethod.Korean.2SetKorean':korean=src;break
if not korean:raise RuntimeError('Enabled Korean two-set input source unavailable')
pid=int(sys.argv[1])
executable=subprocess.run(["/bin/ps","-p",str(pid),"-o","comm="],check=True,capture_output=True,text=True).stdout.strip()
if executable.rsplit("/",1)[-1]!="tst_keefi_local_window":raise RuntimeError("Only the isolated KEEFI test executable is allowed")
# Only target the synthetic test executable by PID, never a user app or vault.
focus=f'tell application "System Events" to set frontmost of first application process whose unix id is {pid} to true'
subprocess.run(['/usr/bin/osascript','-e',focus],check=True,capture_output=True)
try:
    if hi.TISSelectInputSource(korean)!=0:raise RuntimeError('Korean input-source selection failed')
    time.sleep(.3)
    # Physical keys, not Unicode insertion: Abc123!Native.
    actions=[(0,True),(11,False),(8,False),(18,False),(19,False),(20,False),(18,True),(45,True),(0,False),(17,False),(34,False),(9,False),(14,False)]
    body=['tell application "System Events"',f'if (unix id of first application process whose frontmost is true) is not {pid} then error "Probe lost focus"']
    for code,shift in actions:
        body += [f'key code {code}'+(' using shift down' if shift else ''),'delay 0.12']
    body+=['end tell']
    result=subprocess.run(['/usr/bin/osascript','-e','\n'.join(body)],capture_output=True)
    if result.returncode:raise RuntimeError(result.stderr.decode())
finally:
    hi.TISSelectInputSource(previous)
    cf.CFRelease(previous);cf.CFRelease(sources)
