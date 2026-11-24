#pragma once
#include "keefi/BrokerAdapter.h"
#include <QElapsedTimer>
#include <map>
#include <functional>

namespace keefi {
// Controller-owned, single-thread session. Account selectors, keys and cursors
// never enter UI-safe results. A configured account is not a verified account.
class KisAdapter final : public BrokerAdapter {
public:
    explicit KisAdapter(QString environment, std::function<QDateTime()> utcNow = QDateTime::currentDateTimeUtc);
    ~KisAdapter() override;
    QString provider() const override { return QStringLiteral("kis"); }
    BrokerPolicy policy() const override;
    QDateTime utcNow() const override { return utcNow_(); }
    bool symbolValid(const QString&) const override;
    bool catalogSymbolValid(const QString&) const override;
    bool orderReferenceValid(const QString&) const override;
    static QString orderReference(const QString& date, const QString& branch, const QString& number, const QString& venue);
    bool configureSession(const AccountRecord&, const CredentialSet&, QString*) override;
    QString credentialIdentityFingerprint(const CredentialSet&) const override;
    QList<BrokerStep> plan(BrokerPlanKind, const OrderInput&) const override;
    BrokerStep directStep(Operation, const OrderInput&) const override;
    RequestSpec buildStep(const BrokerStep&, qint64 nowMs, QString*) const override;
    BrokerStepResult parseStep(const BrokerStep&, const HttpResult&) const override;
    BrokerReadiness actionReadiness(BrokerAction, const OrderInput&, const QList<BrokerEvidence>&, const OrderLimits&) const override;
    QList<Operation> refreshPlan() const override;
    RequestSpec build(Operation, const AccountRecord&, const OrderInput&, const CredentialSet&, qint64, QString*) const override;
    AdapterResult parse(Operation, const HttpResult&, const OrderInput&) const override;
    OrderValidation validate(const OrderInput&, const MarketSnapshot&, const OrderLimits&) const override;
    QString amendDescription() const override;
    bool sessionReady() const override;
    QList<BrokerAccountChoice> accountChoices() const override;
    bool selectBrokerAccount(const QString&, QString*) override;
    QString selectedAccountScope() const override;
    QString selectedAccountLabel() const override;
    bool restoreReconciliationIntent(const QString&, const OrderInput&) override;
    BrokerReconciliation reconcile(const QString&, const OrderInput&, const QList<OrderRecord>&) const override;
    void clearSession() override;
private:
    struct Cursor {
        SecureBuffer fk, nk;
        Operation operation = Operation::Account;
        QString variant;
        QByteArray binding;
    };
    void clearState() const;
    bool bindMutationTarget(const QString& operation, const QString& original) const;
    QString origin() const;
    QString environment_;
    std::function<QDateTime()> utcNow_;
    mutable SecureBuffer appKey_, appSecret_, cano_, product_, token_;
    mutable QElapsedTimer tokenClock_;
    mutable qint64 lifetimeMs_ = 0;
    mutable QString handle_, scope_, label_;
    mutable bool selected_ = false;
    mutable std::map<QString, Cursor> cursors_;
    mutable QString mutationDate_;
    // Received receipts, or persisted receipts supplied by the controller only
    // after it verifies journal ownership of the selected account.
    mutable QMap<QString, QString> mutationTargets_;
    mutable QMap<QString, MarketInstrument> catalog_;
};
} // namespace keefi
