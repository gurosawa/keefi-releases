#include "PlatformSecurity.h"

#include <QCoreApplication>
#include <QFileInfo>

#if defined(Q_OS_MACOS)
#include <sys/resource.h>
#elif defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifdef __MINGW32__
// The Qt MinGW kit's werapi.h is incomplete. Both functions are exported by Kernel32.
extern "C" __declspec(dllimport) HRESULT WINAPI WerSetFlags(DWORD);
extern "C" __declspec(dllimport) HRESULT WINAPI WerGetFlags(HANDLE, PDWORD);
#ifndef WER_FAULT_REPORTING_FLAG_NOHEAP
constexpr DWORD WER_FAULT_REPORTING_FLAG_NOHEAP = 1;
#endif
#else
#include <werapi.h>
#endif
#include <wtsapi32.h>
#endif

namespace keefi {

#if defined(Q_OS_MACOS)
void* installMacSecurityObservers(PlatformSecurity* owner);
void removeMacSecurityObservers(void* native);
bool setMacKeepAwake(void* native, bool enabled);
#elif defined(Q_OS_WIN)
namespace {
struct WindowsHooks { HWND window = nullptr; };
LRESULT CALLBACK securityWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* owner = reinterpret_cast<PlatformSecurity*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    } else if (owner && message == WM_WTSSESSION_CHANGE &&
               (wparam == WTS_SESSION_LOCK || wparam == WTS_CONSOLE_DISCONNECT ||
                wparam == WTS_REMOTE_DISCONNECT || wparam == WTS_SESSION_LOGOFF)) {
        owner->requestLock(QStringLiteral("session_locked"));
    } else if (owner && message == WM_POWERBROADCAST &&
               (wparam == PBT_APMSUSPEND || wparam == PBT_APMRESUMEAUTOMATIC)) {
        owner->requestLock(QStringLiteral("system_sleep_or_resume"));
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

// Conservative: the presence of a global or applicable executable key blocks
// secrets, including empty keys (Windows defaults apply). Access failures block.
bool localDumpsAbsent(HKEY hive, REGSAM view, const std::wstring& path) {
    HKEY key = nullptr;
    const LSTATUS result = RegOpenKeyExW(hive, path.c_str(), 0, KEY_READ | view, &key);
    if (result == ERROR_SUCCESS) RegCloseKey(key);
    return result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND;
}
} // namespace
#endif

PlatformSecurity::PlatformSecurity(QObject* parent) : QObject(parent) {}

PlatformSecurity::~PlatformSecurity() {
    setKeepAwake(false);
#if defined(Q_OS_MACOS)
    removeMacSecurityObservers(native_);
#elif defined(Q_OS_WIN)
    auto* hooks = static_cast<WindowsHooks*>(native_);
    if (hooks) {
        WTSUnRegisterSessionNotification(hooks->window);
        DestroyWindow(hooks->window);
        delete hooks;
    }
#endif
}

bool PlatformSecurity::hardenCurrentProcess(QString* error) {
    if (error) error->clear();
#if defined(Q_OS_MACOS)
    const rlimit disabled{0, 0};
    rlimit actual{};
    if (setrlimit(RLIMIT_CORE, &disabled) != 0 || getrlimit(RLIMIT_CORE, &actual) != 0 ||
        actual.rlim_cur != 0 || actual.rlim_max != 0) {
        if (error) *error = QStringLiteral("core_dump_limit_failed");
        return false;
    }
    return true;
#elif defined(Q_OS_WIN)
    if (FAILED(WerSetFlags(WER_FAULT_REPORTING_FLAG_NOHEAP))) {
        if (error) *error = QStringLiteral("wer_heap_limit_failed");
        return false;
    }
    DWORD flags = 0;
    if (FAILED(WerGetFlags(GetCurrentProcess(), &flags)) || !(flags & WER_FAULT_REPORTING_FLAG_NOHEAP)) {
        if (error) *error = QStringLiteral("wer_heap_limit_unverified");
        return false;
    }
    const std::wstring base = L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps";
    for (HKEY hive : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
            if (!localDumpsAbsent(hive, view, base)) {
                if (error) *error = QStringLiteral("local_dumps_enabled_or_unreadable");
                return false;
            }
        }
    }
    return true;
#else
    if (error) *error = QStringLiteral("platform_secret_protection_unsupported");
    return false;
#endif
}

bool PlatformSecurity::initialize() {
    ready_ = false;
    if (!hardenCurrentProcess(&status_)) {
        emit statusChanged();
        return false;
    }
    if (!QCoreApplication::instance()) {
        status_ = QStringLiteral("application_not_initialized");
        emit statusChanged();
        return false;
    }
#if defined(Q_OS_MACOS)
    if (!native_) native_ = installMacSecurityObservers(this);
#elif defined(Q_OS_WIN)
    if (!native_) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = securityWindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"KEEFISecurityEvents";
        const ATOM atom = RegisterClassW(&wc);
        if (atom || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            // A hidden top-level window receives broadcast power notifications.
            HWND window = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                           nullptr, nullptr, wc.hInstance, this);
            if (window && WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION))
                native_ = new WindowsHooks{window};
            else if (window) DestroyWindow(window);
        }
    }
#endif
    ready_ = native_ != nullptr;
    status_ = ready_ ? QStringLiteral("ready") : QStringLiteral("session_observer_unavailable");
    emit statusChanged();
    return ready_;
}

bool PlatformSecurity::setKeepAwake(bool enabled) {
    if (enabled && !ready_) return false;
    if (enabled == keep_awake_) return true;
    bool success = false;
#if defined(Q_OS_MACOS)
    success = native_ && setMacKeepAwake(native_, enabled);
#elif defined(Q_OS_WIN)
    success = SetThreadExecutionState(enabled ? ES_CONTINUOUS | ES_SYSTEM_REQUIRED : ES_CONTINUOUS) != 0;
#endif
    if (success) { keep_awake_ = enabled; emit statusChanged(); }
    return success;
}

void PlatformSecurity::requestLock(const QString& reason) {
    setKeepAwake(false);
    emit lockRequested(reason);
}

} // namespace keefi
