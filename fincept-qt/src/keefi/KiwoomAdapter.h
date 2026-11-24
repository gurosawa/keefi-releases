#pragma once
#include "keefi/BrokerAdapter.h"
#include <QElapsedTimer>
#include <QSet>
#include <functional>
#include <map>
namespace keefi {
// One controller thread owns this session; never share the adapter across accounts.
class KiwoomAdapter final : public BrokerAdapter {
public:
    explicit KiwoomAdapter(QString environment, std::function<QDateTime()> clock=QDateTime::currentDateTimeUtc);
    ~KiwoomAdapter() override;
    QString provider() const override { return "kiwoom"; }
    BrokerPolicy policy() const override;
    QDateTime utcNow() const override { return clock_(); }
    bool symbolValid(const QString&) const override;
    bool catalogSymbolValid(const QString&) const override;
    bool orderReferenceValid(const QString&) const override;
    static QString orderReference(const QString& date,const QString& venue,const QString& number);
    bool configureSession(const AccountRecord&,const CredentialSet&,QString*) override;
    QString credentialIdentityFingerprint(const CredentialSet&) const override;
    QList<BrokerStep> plan(BrokerPlanKind,const OrderInput&) const override;
    BrokerStep directStep(Operation,const OrderInput&) const override;
    RequestSpec buildStep(const BrokerStep&,qint64,QString*) const override;
    BrokerStepResult parseStep(const BrokerStep&,const HttpResult&) const override;
    RequestSpec buildPreparedStep(const BrokerStep&,const QList<BrokerEvidence>&,qint64,QString*) const override;
    BrokerStepResult parsePreparedStep(const BrokerStep&,const QList<BrokerEvidence>&,const HttpResult&) const override;
    BrokerReadiness actionReadiness(BrokerAction,const OrderInput&,const QList<BrokerEvidence>&,const OrderLimits&) const override;
    QList<Operation> refreshPlan() const override;
    RequestSpec build(Operation,const AccountRecord&,const OrderInput&,const CredentialSet&,qint64,QString*) const override;
    AdapterResult parse(Operation,const HttpResult&,const OrderInput&) const override;
    OrderValidation validate(const OrderInput&,const MarketSnapshot&,const OrderLimits&) const override;
    QString amendDescription() const override;
    bool sessionReady() const override;
    bool withMarketAuthorization(const MarketAuthorizationBorrow&) const override;
    QList<BrokerAccountChoice> accountChoices() const override;
    bool selectBrokerAccount(const QString&,QString*) override;
    QString selectedAccountScope() const override;
    QString selectedAccountLabel() const override;
    bool restoreReconciliationIntent(const QString&,const OrderInput&) override;
    BrokerReconciliation reconcile(const QString&,const OrderInput&,const QList<OrderRecord>&) const override;
    void clearSession() override;
private:
    struct Cursor { SecureBuffer key; QByteArray binding; int page=1; };
    struct Classification { QMap<QString,MarketInstrument> items; QElapsedTimer age; QJsonObject source; };
    struct Relation { QString target,root; };
    QString origin() const;
    void clearState() const;
    bool bindRelation(const QString&,const QString&,const QString&) const;
    QByteArray stepBinding(const BrokerStep&) const;
    QString valuationPrice(const BrokerStep&,const QList<BrokerEvidence>&) const;
    QString environment_;
    std::function<QDateTime()> clock_;
    mutable SecureBuffer key_,secret_,token_,account_,assertion_;
    mutable QElapsedTimer tokenAge_;
    mutable qint64 lifetimeMs_=0;
    mutable quint64 marketSessionRevision_=1;
    mutable QString handle_,scope_,label_,mutationDate_;
    mutable bool selected_=false;
    mutable std::map<QString,Cursor> cursors_;
    mutable QSet<QByteArray> seenCursorKeys_;
    mutable QMap<QString,Classification> classification_;
    mutable QMap<QString,Relation> relations_;
};
}
