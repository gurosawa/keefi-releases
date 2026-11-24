#pragma once

#include "keefi/TradingTypes.h"
#include <QDateTime>
#include <QByteArrayView>
#include <functional>

namespace keefi {

// A random session handle and masked label only; never a broker account number
// or sequence. The adapter owns the mapping and destroys it with its session.
struct BrokerAccountChoice {
    QString handle;
    QString label;
};

struct BrokerReconciliation {
    bool resolved = false;
    bool applied = false;
    QString message; // App-owned text only, never a raw server error.
};

enum class BrokerAction { Read, Place, Cancel, Amend };
enum class BrokerPlanKind { Authenticate, AccountRead, Catalog, OrderPreflight, CancelPreflight };
enum class BrokerAccountMode { Discovered, Configured };
enum class ExplicitQtyBasis { Total, Remaining };
enum class BrokerReferenceModel { TargetReceipt, TargetReceiptRoot };
enum class BrokerAuthScope { Public, CredentialBootstrap, Session, SelectedAccount };
enum class ApiAvailability { Documented, NotDocumented, ExplicitlyUnsupported };
enum class SourceTimeQuality { FullTimestamp, TimeOfDayOnly, NotProvided };
enum class BrokerNotReady { None, UnsupportedApi, NotImplemented, NotAuthenticated, AccountNotVerified,
    Busy, MissingEvidence, ExpiredEvidence, ChangedInput, PartialCoverage, UnresolvedOrder, RejectedBySafetyRule,
    RequestSpacing, RateLimited, ReauthenticationRequired };
struct BrokerFeature {
    ApiAvailability api = ApiAvailability::NotDocumented;
    bool implemented = false;
    QString explanation;
};
struct BrokerFreshnessRule {
    Operation operation = Operation::Quote;
    qint64 maxReceiptAgeMs = 10000;
    SourceTimeQuality acceptedSourceQuality = SourceTimeQuality::NotProvided;
    bool requiredForAction = true;
    qint64 maxSourceAgeMs = 10000;
};
struct BrokerPolicy {
    QString provider, displayName, environment, market = "domestic_equity", currency = "KRW";
    BrokerAccountMode accountMode = BrokerAccountMode::Discovered;
    QMap<BrokerAction, BrokerFeature> actions;
    BrokerFeature catalog, officialOrderTest, paperTrading;
    QMap<BrokerAction, QList<BrokerFreshnessRule>> freshness;
    bool exactIntentPreflight = false, publicCatalog = false;
    bool partialFillAmend = false;
    Operation quoteOperation = Operation::Quote;
    QString catalogSource, holdingsSource, buyingPowerSource;
    OrderQuantityMode cancelQuantityMode = OrderQuantityMode::Exact;
    OrderQuantityMode amendQuantityMode = OrderQuantityMode::Exact;
    QString accountSource;
    ExplicitQtyBasis cancelExplicitQtyBasis = ExplicitQtyBasis::Total;
    bool amendPriceOnly = false;
    QStringList placeOrderTypes{QStringLiteral("limit"), QStringLiteral("market")};
    BrokerReferenceModel referenceModel = BrokerReferenceModel::TargetReceipt;
};
struct BrokerStepDependency {
    Operation operation = Operation::Quote;
    QString variant;
    qint64 maxReceiptAgeMs = 10000;
};
struct BrokerStep {
    Operation operation = Operation::Quote;
    BrokerAuthScope authScope = BrokerAuthScope::SelectedAccount;
    QString variant; // Adapter constant, never a user-provided endpoint/TR.
    OrderInput input;
    QString cursorHandle; // Opaque private pagination handle, never account selectors.
    int minimumDelayMs = 300;
    QString catalogMarket;
    bool allowEmptyCatalog = false;
    QList<BrokerStepDependency> dependencies;
    // Only public catalog enrichment may be nonfatal. A failed auxiliary source
    // must invalidate its private classification cache before parsing returns.
    bool catalogAuxiliary = false;
    // Only selected-account History inside AccountRead may degrade to partial
    // coverage. Never relax preflight, reconciliation, auth or throttling.
    bool optionalAccountRead = false;
};
struct BrokerSourceTime {
    SourceTimeQuality quality = SourceTimeQuality::NotProvided;
    QString rawText, fieldName;
    QDateTime fullTimestamp; // Invalid unless the source supplies a date and time.
};
struct BrokerStepResult {
    AdapterResult value;
    BrokerSourceTime sourceTime;
    bool accountReadVerified = false;
    bool mutationRejected = false; // Only a documented, definitive broker rejection; never a transport inference.
};
struct BrokerEvidence {
    Operation operation = Operation::Quote;
    QString variant;
    quint64 sessionGeneration = 0, draftRevision = 0;
    QString accountScope;
    QByteArray inputBinding;
    qint64 receivedMonotonicMs = -1;
    QDateTime receivedUtc;
    BrokerSourceTime sourceTime;
    bool complete = false;
    QJsonObject data;
};
struct BrokerReadiness {
    bool ready = false;
    BrokerNotReady reason = BrokerNotReady::MissingEvidence;
    QString message;
    OrderValidation validation; // Exact preflight adapters return the validated value bound to this intent.
};

class BrokerAdapter : public ExchangeAdapter {
  public:
    // Synchronous read-only borrow; the callback must not retain the byte view.
    // Reuses an existing session and never authenticates or exposes a UI token.
    using MarketAuthorizationBorrow = std::function<void(QByteArrayView, qint64, quint64)>;
    virtual bool withMarketAuthorization(const MarketAuthorizationBorrow&) const { return false; }
    virtual QString marketDataCountry(const QString&) const { return {}; }
    virtual QStringList marketDataHoldings() const { return {}; }
    virtual BrokerPolicy policy() const = 0;
    virtual bool symbolValid(const QString&) const = 0;
    virtual bool catalogSymbolValid(const QString& symbol) const { return symbolValid(symbol); }
    virtual bool orderReferenceValid(const QString&) const = 0;
    // Called only inside a synchronous Vault credential borrow. No network.
    virtual bool configureSession(const AccountRecord&, const CredentialSet&, QString* error) = 0;
    virtual QString credentialIdentityFingerprint(const CredentialSet&) const = 0;
    virtual QList<BrokerStep> plan(BrokerPlanKind, const OrderInput&) const = 0;
    // Query/mutation jobs originate in the controller, outside read plans.
    virtual BrokerStep directStep(Operation operation, const OrderInput& input) const {
        BrokerStep step; step.operation = operation; step.input = input; return step;
    }
    virtual RequestSpec buildStep(const BrokerStep&, qint64 nowMs, QString* error) const = 0;
    virtual BrokerStepResult parseStep(const BrokerStep&, const HttpResult&) const = 0;
    virtual RequestSpec buildPreparedStep(const BrokerStep& step, const QList<BrokerEvidence>& usedDependencies,
        qint64 nowMs, QString* error) const {
        if (step.dependencies.isEmpty() && usedDependencies.isEmpty()) return buildStep(step, nowMs, error);
        if (error) *error = QStringLiteral("사전 조회 의존성 처리가 구현되지 않았습니다.");
        return {};
    }
    virtual BrokerStepResult parsePreparedStep(const BrokerStep& step, const QList<BrokerEvidence>& usedDependencies,
        const HttpResult& response) const {
        if (step.dependencies.isEmpty() && usedDependencies.isEmpty()) return parseStep(step, response);
        BrokerStepResult result; result.value.message = QStringLiteral("사전 조회 의존 응답을 확인할 수 없습니다.");
        return result;
    }
    virtual QDateTime utcNow() const { return QDateTime::currentDateTimeUtc(); }
    virtual BrokerReadiness actionReadiness(BrokerAction, const OrderInput&,
        const QList<BrokerEvidence>&, const OrderLimits&) const = 0;
    virtual bool sessionReady() const = 0;
    virtual QList<BrokerAccountChoice> accountChoices() const = 0;
    virtual bool selectBrokerAccount(const QString& handle, QString* error) = 0;
    virtual QString selectedAccountScope() const = 0;
    virtual QString selectedAccountLabel() const = 0;
    virtual bool supportsOfficialOrderTest() const { return false; }
    virtual bool supportsClientIdQuery() const { return false; }
    // The controller calls this only for a persisted broker receipt belonging
    // to the currently verified account. A user-supplied association is never
    // passed here. Restoring context cannot itself resolve an order: official
    // query responses must still prove the relationship and original intent.
    virtual bool restoreReconciliationIntent(const QString&, const OrderInput&) { return true; }
    virtual BrokerReconciliation reconcile(const QString& kind, const OrderInput& intent,
                                           const QList<OrderRecord>& observed) const = 0;
    // Authentication/account discovery never count as a completed account refresh.
    // Token and private account selectors must not be returned in AdapterResult.
    virtual void clearSession() = 0;
};

} // namespace keefi
