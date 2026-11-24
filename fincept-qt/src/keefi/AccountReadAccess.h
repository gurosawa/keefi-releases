#pragma once
#include "keefi/Vault.h"

namespace keefi {
// Local availability only: this is not proof of API authentication/permission.
// withCredentials also rejects the aboutToLock phase and inaccessible records.
inline bool accountCredentialsAvailable(const Vault& vault,const QString& id) {
    return vault.withCredentials(id,[](const CredentialSet&){});
}
// Personal account data is available only while the vault is unlocked.
inline bool accountReadsAvailable(const Vault& vault) { return vault.isUnlocked(); }
}
