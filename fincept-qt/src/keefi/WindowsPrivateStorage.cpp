#include "WindowsPrivateStorage.h"
#include <QDir>
#include <QFileInfo>
#include <qt_windows.h>
#include <aclapi.h>
#include <vector>

namespace keefi {
namespace {
bool denied(QString* error, const char* stage, DWORD code = ERROR_SUCCESS) {
    if (error)
        *error = QStringLiteral("Windows 저장소 권한을 확인하지 못했습니다 (%1/%2).")
                    .arg(QLatin1String(stage)).arg(code);
    return false;
}

struct UserPermissions {
    std::vector<unsigned char> tokenInfo;
    PSID sid = nullptr;
    PACL acl = nullptr;
    ~UserPermissions() { if (acl) LocalFree(acl); }
    bool identify(QString* error) {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return denied(error, "open-token", GetLastError());
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        tokenInfo.resize(size);
        const bool ok = GetTokenInformation(token, TokenUser, tokenInfo.data(), size, &size);
        const DWORD status = ok ? ERROR_SUCCESS : GetLastError();
        CloseHandle(token);
        if (!ok) return denied(error, "read-token", status);
        sid = reinterpret_cast<TOKEN_USER*>(tokenInfo.data())->User.Sid;
        return true;
    }
    bool makeAcl(QString* error) {
        EXPLICIT_ACCESSW entry{};
        entry.grfAccessPermissions = FILE_ALL_ACCESS;
        entry.grfAccessMode = SET_ACCESS;
        entry.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
        entry.Trustee.ptstrName = static_cast<LPWSTR>(sid);
        const DWORD status = SetEntriesInAclW(1, &entry, nullptr, &acl);
        return status == ERROR_SUCCESS || denied(error, "make-acl", status);
    }
};
}

bool privateWindowsPath(const QString& path, bool newlyCreated, QString* error) {
    UserPermissions user;
    if (!user.identify(error)) return false;
    auto native = QDir::toNativeSeparators(path).toStdWString();
    if (newlyCreated) {
        if (!user.makeAcl(error)) return false;
        const DWORD status = SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            user.sid, nullptr, user.acl, nullptr);
        if (status != ERROR_SUCCESS) return denied(error, "set-new-owner-dacl", status);
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr;
    PACL acl = nullptr;
    const DWORD status = GetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &acl, nullptr, &descriptor);
    if (status != ERROR_SUCCESS) return denied(error, "read-owner-dacl", status);
    const bool owns = owner && EqualSid(owner, user.sid);
    bool valid = acl != nullptr;
    for (DWORD i = 0; valid && i < acl->AceCount; ++i) {
        void* ace = nullptr;
        if (!GetAce(acl, i, &ace)) { valid = false; break; }
        const auto type = static_cast<ACE_HEADER*>(ace)->AceType;
        if (type == ACCESS_ALLOWED_ACE_TYPE) {
            const auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
            valid = EqualSid(const_cast<DWORD*>(&allowed->SidStart), user.sid);
        } else if (type != ACCESS_DENIED_ACE_TYPE) {
            valid = false;
        }
    }
    LocalFree(descriptor);
    if (!owns) return denied(error, "owner-mismatch");
    return valid || denied(error, "nonprivate-dacl");
}

bool createPrivateWindowsDirectory(const QString& path, QString* error) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return denied(error, "create-parent");
    UserPermissions user;
    if (!user.identify(error) || !user.makeAcl(error)) return false;
    SECURITY_DESCRIPTOR descriptor{};
    if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION)
        || !SetSecurityDescriptorOwner(&descriptor, user.sid, FALSE)
        || !SetSecurityDescriptorDacl(&descriptor, TRUE, user.acl, FALSE)
        || !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
        return denied(error, "prepare-directory-acl", GetLastError());
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
    const auto native = QDir::toNativeSeparators(path).toStdWString();
    if (!CreateDirectoryW(native.c_str(), &attributes)) {
        const DWORD status = GetLastError();
        if (status != ERROR_ALREADY_EXISTS) return denied(error, "create-directory", status);
    }
    // A concurrently created directory is validated without changing its owner or ACL.
    return true;
}
}
