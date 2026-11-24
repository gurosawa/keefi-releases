#pragma once

#include <QByteArray>
#include <QString>

namespace keefi {

// A profile keeps its logical identifiers across a storage-container rewrite.
// The process hosts one profile; this selection is fixed before any workers start.
enum class IdentityScheme { KeefiV1, LegacyV1 };
enum class IdentityDomain {
    CurrentAssetsCredentialBinding,
    EventReference,
    PortableRecordScope,
    ValuationPreferences,
    BrokerAccountIdentity,
    BrokerConfiguredIdentity,
    KisCredentialIdentity,
    KisHoldingIdentity,
    KiwoomAccount,
    KiwoomCredentials,
    KiwoomCursor,
    KiwoomPosition,
    LsAccount,
    LsCredentials,
    LsCursor,
    LsPosition,
    OkxAccount,
    SolanaMainnet,
    HyperliquidMainnet,
    Count
};

// Invalid names and attempts to switch an already selected profile fail closed.
bool configureIdentityScheme(const QString& name, QString* error = nullptr);
QString identitySchemeName();
QByteArray identityDomain(IdentityDomain domain);
QString identityDomainText(IdentityDomain domain);

} // namespace keefi
