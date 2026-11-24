#include "keefi/IdentityCompatibility.h"
#include "keefi/ValuationPreferences.h"
#include "keefi/ProfileSettings.h"
#include "keefi/SecureBuffer.h"
#include "keefi/SolanaTransactionCodec.h"
#include "keefi/Vault.h"

#include <QCryptographicHash>
#include <QSettings>
#include <QStringList>

namespace keefi {
namespace {
bool fail(QString* error, const QString& message) {
    if (error) *error = message;
    return false;
}
bool validAsset(const QString& value) {
    return value.startsWith("solana:") && SolanaTransactionCodec::publicKeyValid(value.mid(7));
}
QString settingsKey(const QString& binding) { return "valuation/excludedMints/" + binding; }
void renewSettings(std::unique_ptr<QSettings>& settings) {
    // QSettings::status remembers its first error for the object's lifetime.
    // Reopen the same channel so a later retry can succeed after storage recovers.
    auto fresh = std::make_unique<QSettings>(settings->format(), settings->scope(), settings->organizationName(), settings->applicationName());
    fresh->setFallbacksEnabled(false);
    settings = std::move(fresh);
}
bool readMints(QSettings& settings, const QString& key, QSet<QString>* assets, QString* error) {
    assets->clear();
    if (!settings.contains(key)) return true;
    const auto value = settings.value(key);
    if (value.metaType() != QMetaType::fromType<QStringList>() && value.metaType() != QMetaType::fromType<QString>())
        return fail(error, QObject::tr("평가 제외 설정의 형식을 확인하지 못했습니다."));
    for (const auto& mint : value.toStringList()) {
        const auto asset = "solana:" + mint;
        if (!validAsset(asset)) return fail(error, QObject::tr("평가 제외 설정에 올바르지 않은 Mint가 있습니다."));
        assets->insert(asset);
    }
    return true;
}
} // namespace

ValuationPreferences::ValuationPreferences(Vault& vault, QObject* parent)
    : QObject(parent), vault_(vault), settings_(std::make_unique<ProfileSettings>()) {
    // Capture this application's settings channel at construction, including in
    // tests which create separate production/development channels in one process.
    settings_->setFallbacksEnabled(false);
    connect(&vault_, &Vault::aboutToLock, this, &ValuationPreferences::clear);
    connect(&vault_, &Vault::stateChanged, this, [this] {
        if (vault_.isUnlocked()) load();
        else clear();
    });
    if (vault_.isUnlocked()) load();
}
ValuationPreferences::~ValuationPreferences() = default;

QString ValuationPreferences::binding(bool createIfMissing, QString* error) {
    QString result;
    if (!vault_.withAssetsLedgerKey(createIfMissing, [&](const SecureBuffer& key) {
            QCryptographicHash digest(QCryptographicHash::Sha256);
            digest.addData(identityDomain(IdentityDomain::ValuationPreferences));
            digest.addData(QByteArrayView(reinterpret_cast<const char*>(key.data()), qsizetype(key.size())));
            result = QString::fromLatin1(digest.result().toHex());
        }, error)) return {};
    return result;
}

bool ValuationPreferences::load(QString* error) {
    if (error) error->clear();
    if (!vault_.isUnlocked()) {
        clear();
        return fail(error, tr("평가 제외 설정을 읽으려면 금고를 열어 주세요."));
    }
    QString keyError;
    const auto nextBinding = binding(false, &keyError);
    if (nextBinding.isEmpty()) {
        clear();
        // withAssetsLedgerKey(false) has no write path. A missing key returns
        // false while the vault stays open; protection errors lock the vault.
        if (vault_.isUnlocked()) { loaded_ = true; return true; }
        return fail(error, keyError);
    }
    settings_->sync();
    if (settings_->status() != QSettings::NoError) {
        renewSettings(settings_);
        clear();
        return fail(error, tr("평가 제외 설정을 읽지 못했습니다."));
    }
    QSet<QString> next;
    if (!readMints(*settings_, settingsKey(nextBinding), &next, error)) { clear(); return false; }
    const bool different = excluded_ != next;
    binding_ = nextBinding;
    excluded_ = std::move(next);
    loaded_ = true;
    if (different) emit changed();
    return true;
}

QSet<QString> ValuationPreferences::excludedAssets() const { return vault_.isUnlocked() ? excluded_ : QSet<QString>{}; }

bool ValuationPreferences::setExcluded(const QString& canonicalAsset, bool excluded, QString* error) {
    if (error) error->clear();
    if (!vault_.isUnlocked()) return fail(error, tr("금고를 연 뒤 평가 제외를 변경할 수 있습니다."));
    if (!validAsset(canonicalAsset)) return fail(error, tr("평가 제외할 Solana Mint를 확인해 주세요."));
    if (!loaded_ && !load(error)) return false;
    if (binding_.isEmpty()) {
        if (!excluded) return true; // Restoring an unset choice never creates a ledger key.
        binding_ = binding(true, error);
        if (binding_.isEmpty()) return false;
    }
    settings_->sync();
    if (settings_->status() != QSettings::NoError) {
        renewSettings(settings_);
        return fail(error, tr("평가 제외 설정을 저장할 수 없습니다."));
    }
    const auto key = settingsKey(binding_);
    QSet<QString> next;
    if (!readMints(*settings_, key, &next, error)) return false;
    const auto before = next;
    if (excluded) next.insert(canonicalAsset);
    else next.remove(canonicalAsset);
    if (before != next) {
        const bool existed = settings_->contains(key);
        const auto previousValue = settings_->value(key);
        QStringList mints;
        for (const auto& asset : next) mints.append(asset.mid(7));
        mints.sort();
        if (mints.isEmpty()) settings_->remove(key);
        else settings_->setValue(key, mints);
        settings_->sync();
        if (settings_->status() != QSettings::NoError) {
            // Restore QSettings' pending value as well as keeping the UI cache.
            // The original disk data is not replaced by an unconfirmed choice.
            if (existed) settings_->setValue(key, previousValue);
            else settings_->remove(key);
            settings_->sync();
            renewSettings(settings_);
            return fail(error, tr("평가 제외 설정을 저장하지 못했습니다. 기존 선택을 유지합니다."));
        }
    }
    const bool different = excluded_ != next;
    excluded_ = std::move(next);
    if (different) emit changed();
    return true;
}

void ValuationPreferences::clear() {
    const bool hadChoices = !excluded_.isEmpty();
    excluded_.clear();
    binding_.clear();
    loaded_ = false;
    if (hadChoices) emit changed();
}
} // namespace keefi
