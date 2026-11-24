#pragma once
#include "keefi/BrokerAdapter.h"
#include <QElapsedTimer>
#include <QSet>
#include <functional>
#include <map>
namespace keefi {
class LsAdapter final : public BrokerAdapter {
public:
    explicit LsAdapter(QString environment, std::function<QDateTime()> clock=QDateTime::currentDateTimeUtc);
    ~LsAdapter() override;
    QString provider() const override { return "ls"; }
    BrokerPolicy policy() const override;
    QDateTime utcNow() const override { return clock_(); }
    bool symbolValid(const QString&) const override;
    bool catalogSymbolValid(const QString&) const override;
    bool orderReferenceValid(const QString&) const override;
    static QString orderReference(const QString&,const QString&,const QString&);
    bool configureSession(const AccountRecord&,const CredentialSet&,QString*) override;
    QString credentialIdentityFingerprint(const CredentialSet&) const override;
    QList<BrokerStep> plan(BrokerPlanKind,const OrderInput&) const override;
    BrokerStep directStep(Operation,const OrderInput&) const override;
    RequestSpec buildStep(const BrokerStep&,qint64,QString*) const override;
    BrokerStepResult parseStep(const BrokerStep&,const HttpResult&) const override;
    BrokerReadiness actionReadiness(BrokerAction,const OrderInput&,const QList<BrokerEvidence>&,const OrderLimits&) const override;
    QList<Operation> refreshPlan() const override;
    RequestSpec build(Operation,const AccountRecord&,const OrderInput&,const CredentialSet&,qint64,QString*) const override;
    AdapterResult parse(Operation,const HttpResult&,const OrderInput&) const override;
    OrderValidation validate(const OrderInput&,const MarketSnapshot&,const OrderLimits&) const override;
    QString amendDescription() const override;
    bool sessionReady() const override;
    QList<BrokerAccountChoice> accountChoices() const override;
    bool selectBrokerAccount(const QString&,QString*) override;
    QString selectedAccountScope() const override;
    QString selectedAccountLabel() const override;
    bool restoreReconciliationIntent(const QString&,const OrderInput&) override;
    BrokerReconciliation reconcile(const QString&,const OrderInput&,const QList<OrderRecord>&) const override;
    void clearSession() override;
private:
    struct Cursor { SecureBuffer key; QJsonObject bodyCursor; QByteArray binding; int page=1; QSet<QString> symbols; QSet<QByteArray> visited; QDateTime firstReceived; QElapsedTimer firstAge; };
    struct Relation { QString target,root; };
    struct Receipt { QString scope,date; OrderInput intent; };
    void clearState() const;
    QByteArray binding(const BrokerStep&) const;
    bool bindRelation(const QString&,const QString&,const QString&) const;
    QString environment_;
    std::function<QDateTime()> clock_;
    mutable SecureBuffer key_,secret_,token_,account_,assertion_;
    mutable QElapsedTimer tokenAge_;
    mutable qint64 lifetimeMs_=0;
    mutable QString handle_,scope_,label_,mutationDate_;
    mutable bool selected_=false;
    mutable std::map<QString,Cursor> cursors_;
    mutable QMap<QString,Relation> relations_;
    mutable QMap<QString,Receipt> receipts_;
    mutable QMap<QString,QJsonObject> catalog_;
};
}
