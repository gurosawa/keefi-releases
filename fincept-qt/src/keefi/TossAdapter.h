#pragma once

#include "keefi/BrokerAdapter.h"
#include <QElapsedTimer>
#include <QHash>
#include <vector>

namespace keefi {

// One controller-owned session. Never share this mutable adapter between threads.
// Authentication and account-selection state stays outside UI-safe results.
class TossAdapter final : public BrokerAdapter {
  public:
    ~TossAdapter() override;
    BrokerPolicy policy() const override;
    bool symbolValid(const QString&) const override;
    bool orderReferenceValid(const QString&) const override;
    bool configureSession(const AccountRecord&, const CredentialSet&, QString* error) override;
    QString credentialIdentityFingerprint(const CredentialSet&) const override;
    QList<BrokerStep> plan(BrokerPlanKind, const OrderInput&) const override;
    RequestSpec buildStep(const BrokerStep&, qint64 nowMs, QString* error) const override;
    BrokerStepResult parseStep(const BrokerStep&, const HttpResult&) const override;
    BrokerReadiness actionReadiness(BrokerAction, const OrderInput&, const QList<BrokerEvidence>&, const OrderLimits&) const override;
    QString provider() const override { return QStringLiteral("toss"); }
    QList<Operation> refreshPlan() const override;
    RequestSpec build(Operation operation, const AccountRecord& account, const OrderInput& input,
                      const CredentialSet& credentials, qint64 serverNowMs, QString* error) const override;
    AdapterResult parse(Operation operation, const HttpResult& response, const OrderInput& input) const override;
    OrderValidation validate(const OrderInput& input, const MarketSnapshot& snapshot,
                             const OrderLimits& limits) const override;
    QString amendDescription() const override;
    bool sessionReady() const override;
    bool withMarketAuthorization(const MarketAuthorizationBorrow&) const override;
    QString marketDataCountry(const QString&) const override;
    QStringList marketDataHoldings() const override;
    QList<BrokerAccountChoice> accountChoices() const override;
    bool selectBrokerAccount(const QString& handle, QString* error) override;
    QString selectedAccountScope() const override;
    QString selectedAccountLabel() const override;
    BrokerReconciliation reconcile(const QString& kind, const OrderInput& intent,
                                   const QList<OrderRecord>& observed) const override;
    void clearSession() override;

  private:
    struct AccountEntry {
        BrokerAccountChoice choice;
        SecureBuffer sequence;
        QString identityScope; // Hash only; raw account number is never retained here.
    };
    void clearSessionState() const;
    AccountRecord configured_;
    mutable SecureBuffer bootstrapId_, bootstrapSecret_;
    mutable SecureBuffer token_, selectedSequence_;
    mutable std::vector<AccountEntry> accounts_;
    mutable QElapsedTimer sessionClock_;
    mutable qint64 sessionLifetimeMs_ = 0;
    mutable quint64 marketSessionRevision_ = 0;
    mutable QString selectedScope_, selectedLabel_;
    // Derived only from official catalogue/detail/holdings metadata, not currency
    // or the shape of a ticker. Contains no account identifiers.
    mutable QHash<QString, QString> symbolMarkets_;
    mutable QStringList marketHoldings_;
    mutable qint64 marketHoldingsAt_ = -1;
};

} // namespace keefi
