#include "IdentityCompatibility.h"
#include "LegacyIdentityPrivate.h"

#include <array>
#include <atomic>
#include <mutex>

namespace keefi {
namespace {
std::atomic<IdentityScheme> scheme{IdentityScheme::KeefiV1};
std::mutex configurationMutex;
bool configured = false;

constexpr std::array<const char*, 19> CurrentDomains{{
    "KEEFI/current-assets/credential-binding/v1",
    "KEEFI/event-reference/v1",
    "KEEFI/portable-record-scope/v1",
    "KEEFI/valuation-preferences/v1",
    "KEEFI/BrokerAccountIdentity/v1",
    "KEEFI/BrokerConfiguredIdentity/v1",
    "KEEFI/KIS/CredentialIdentity/v1",
    "KEEFI/KIS/HoldingIdentity/v1",
    "keefi.kiwoom.account.v1",
    "keefi.kiwoom.credentials.v1",
    "keefi.kiwoom.cursor.v1",
    "keefi.kiwoom.position.v1",
    "keefi.ls.account.v1",
    "keefi.ls.credentials.v1",
    "keefi.ls.cursor.v1",
    "keefi.ls.position.v1",
    "keefi.okx.account.v1",
    "keefi.solana.mainnet:",
    "keefi.hyperliquid.mainnet:"
}};
static_assert(CurrentDomains.size() == std::size_t(IdentityDomain::Count));
static_assert(CurrentDomains.size() == legacy::IdentityDomains.size());
}

bool configureIdentityScheme(const QString& name, QString* error) {
    if (error) error->clear();
    IdentityScheme requested;
    if (name == QStringLiteral("keefi-v1")) requested = IdentityScheme::KeefiV1;
    else if (name == QStringLiteral("legacy-v1")) requested = IdentityScheme::LegacyV1;
    else {
        if (error) *error = QStringLiteral("지원하지 않는 프로필 식별자 형식입니다.");
        return false;
    }
    const std::lock_guard lock(configurationMutex);
    if (configured && scheme.load(std::memory_order_acquire) != requested) {
        if (error) *error = QStringLiteral("실행 중인 프로필의 식별자 형식을 바꿀 수 없습니다.");
        return false;
    }
    scheme.store(requested, std::memory_order_release);
    configured = true;
    return true;
}

QString identitySchemeName() {
    return scheme.load(std::memory_order_acquire) == IdentityScheme::LegacyV1
        ? QStringLiteral("legacy-v1") : QStringLiteral("keefi-v1");
}

QByteArray identityDomain(IdentityDomain domain) {
    const auto index = std::size_t(domain);
    if (index >= CurrentDomains.size()) qFatal("Invalid profile identity domain");
    const auto& domains = scheme.load(std::memory_order_acquire) == IdentityScheme::LegacyV1
        ? legacy::IdentityDomains : CurrentDomains;
    return QByteArray(domains[index]);
}

QString identityDomainText(IdentityDomain domain) {
    return QString::fromLatin1(identityDomain(domain));
}

} // namespace keefi
