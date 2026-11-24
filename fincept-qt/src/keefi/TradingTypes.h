#pragma once
#include "keefi/AccountModel.h"
#include "keefi/ExchangeIdentifiers.h"
#include "keefi/SecureBuffer.h"
#include <QJsonObject>
#include <QJsonDocument>
#include <QMap>
#include <QUrl>
#include <optional>

namespace keefi {
// All financial values and exchange identifiers remain decimal/text strings.
// JSON numbers are converted to their original decimal lexemes before Qt parses.
enum class JsonResponseScope { Standard, BinanceMarketCatalog };
inline constexpr qsizetype MaxStandardResponseBytes = 4 * 1024 * 1024;
inline constexpr qsizetype MaxBinanceMarketCatalogResponseBytes = 16 * 1024 * 1024;
QJsonDocument parseExactJson(const QByteArray& bytes, QString* error = nullptr,
                            JsonResponseScope scope = JsonResponseScope::Standard);
namespace decimal {
bool valid(const QString& value);
QString normalize(const QString& value);
int compare(const QString& a, const QString& b); // valid non-negative inputs only
QString add(const QString& a, const QString& b);
QString subtract(const QString& a, const QString& b); // invalid/negative -> empty
QString multiply(const QString& a, const QString& b);
bool multipleOf(const QString& value, const QString& step);
bool positive(const QString& value);
}

enum class Operation { Time, Markets, Rules, Quote, Depth, Reference, AveragePrice, AssetFilters, Account, OpenOrders,
    History, QueryOrder, TestOrder, PlaceOrder, CancelOrder, AmendOrder,
    Authenticate, Accounts, Holdings, BuyingPower, SellableQuantity, Commissions, PriceLimits, TradingSchedule,
    AccountConfig, FundingBalance, FxUsd, FxUsdt, Positions, SymbolConfig, LeverageBrackets, MarkPrice,
    ApiRestrictions, TradingStatus, AlgoOpenOrders, AlgoHistory, AlgoQueryOrder, AlgoCancelOrder, AmendHistory, OrderList, Income, EarnRead, FundFlowRead };
bool isMutation(Operation operation);
QString operationName(Operation operation);
enum class OrderQuantityMode { Exact, AllRemaining };
enum class OrderReferenceProvenance { None, BrokerReceipt };
enum class OrderFamily { Ordinary, Conditional };
enum class ExposureIntent { Unspecified, Open, Reduce };
struct FuturesOrderTerms {
    OrderFamily family = OrderFamily::Ordinary;
    ExposureIntent exposure = ExposureIntent::Unspecified;
    QString positionSide, timeInForce, workingType, triggerPrice, activationPrice, callbackRate, modifyId;
    bool reduceOnly = false, closePosition = false;
    std::optional<bool> priceProtect; // Missing is unknown for externally observed Algo orders.
    bool operator==(const FuturesOrderTerms&) const = default;
};
struct PositionRecord {
    QString symbol, positionSide, amount, entryPrice, breakEvenPrice, markPrice, liquidationPrice;
    QString unrealizedPnl, notional, marginAsset, marginType, leverage, isolatedWallet, initialMargin, maintMargin, updateTime;
    bool operator==(const PositionRecord&) const = default;
};
struct OrderInput {
    QString symbol, side = "buy", type = "limit";
    QString price, quantity, quoteAmount;
    QString orderId, clientId, newClientId;
    QString operationOrderId; // Broker mutation receipt; distinct from its target order.
    QString cursor;
    qint64 startTimeMs = 0, endTimeMs = 0;
    OrderQuantityMode quantityMode = OrderQuantityMode::Exact;
    QString amendRequestId; // Same-order amendment correlation, distinct from order/client IDs.
    QString baseOriginalOrderId;
    std::optional<FuturesOrderTerms> futures;
    QString algoId, clientAlgoId, actualOrderId;
    bool operator==(const OrderInput&) const = default;
    OrderReferenceProvenance baseOriginalProvenance = OrderReferenceProvenance::None;
    bool cancelReplace = false; // Explicit Binance Spot cancel-and-new intent; new quantity excludes prior fills.
    QString orderListId;
    bool brokerConditional=false; // Native domestic conditional ID, separate from ordinary orders.
};
struct RequestSpec {
    Operation operation = Operation::Time;
    QByteArray method = "GET";
    QUrl url;
    QMap<QByteArray, QByteArray> headers;
    QByteArray body;
    bool authenticated = false;
};
struct HttpResult {
    int status = 0;
    QByteArray body;
    QMap<QByteArray, QByteArray> headers;
    bool timedOut = false, networkError = false;
    // Set only by the local transport before network dispatch. Never inferred
    // from an exchange response; a sent timeout/429 remains adapter-specific.
    int localRetryAfterSeconds = 0;
};
struct Balance {
    QString asset, free, locked;
    QString section, cashBalance, equity, liability; // Empty is unavailable, never implicit zero.
    // Optional provider-reported spot average, for chart display only. Never
    // derive from balances or feed these fields into valuation or the ledger.
    QString providerAveragePrice, providerAverageCurrency;
};
struct MarketInstrument {
    QString symbol, baseAsset, quoteAsset, name, englishName, status;
    // Catalogue eligibility only. Account permissions and current order rules
    // must still be checked before any order is accepted.
    bool tradable = false;
    QString warning;
    QString market;
    QString securityType; // Official instrument/group code, never inferred from its name.
    QString preferredClass, etpClass; // Official master codes, including unknown/blank values.
    bool operator==(const MarketInstrument&) const = default;
};
struct OrderRecord {
    QString orderId, clientId, symbol, side, type, status;
    QString rawStatus, currency, market;
    QString journalId; // Local encrypted-ledger entry handle, never sent to a broker.
    QString price, quantity, executed, quoteAmount, remaining;
    QString originalOrderId, replacementOrderId, replacementClientId;
    bool cancelable = false, amendable = false;
    QString baseOriginalOrderId;
    QString quantityUnit, requestedQuoteAmount, executedBaseAmount, remainingBaseAmount;
    QString baseAsset, quoteAsset, requestState, actionOutcome, amendRequestId, sourceUpdatedAt;
    QString tdMode, instType;
    bool linkedAlgo = false;
    std::optional<FuturesOrderTerms> futures;
    QString algoId, clientAlgoId, actualOrderId;
    QString orderListId;
    bool brokerConditional=false;
    QJsonObject conditions; // Sanitized group/leg terms for explicit review.
    bool operator==(const OrderRecord&) const = default;
};
struct AdapterResult {
    bool ok = false, unknown = false, notFound = false, authenticationFailed = false;
    bool definiteRejection = false; // Provider-verified business rejection, including HTTP 200.
    QString requestState; // accepted/rejected/unknown; not the observed order state.
    bool rateLimited = false, needsReconciliation = false, hasMore = false;
    // No exchange error messages/raw authenticated payloads in these UI-safe fields.
    QString code, message, nextCursor, scope;
    int retryAfterSeconds = 0;
    qint64 serverTimeMs = 0;
    bool tradingPermissionKnown = false, tradingAllowed = false;
    QJsonObject data;
    QList<Balance> balances;
    QList<OrderRecord> orders;
    QList<PositionRecord> positions;
};
inline AdapterResult localRateLimitFailure(const HttpResult& response) {
    AdapterResult result;
    result.rateLimited = true; result.definiteRejection = true;
    result.retryAfterSeconds = response.localRetryAfterSeconds;
    result.requestState = "rejected"; result.code = "local-rate-limit";
    result.message = "같은 서비스의 요청 제한 대기 중이라 이 요청은 전송하지 않았습니다. 자동 재전송하지 않습니다.";
    return result;
}
struct MarketSnapshot {
    qint64 serverNowMs = 0; // Monotonic server-time estimate supplied at validation.
    QMap<Operation, QJsonObject> data;
    QList<Balance> balances;
    QList<OrderRecord> openOrders;
    QList<PositionRecord> positions;
    // Monotonic age is enforced by the service; these are display timestamps.
    qint64 quoteReceivedAtMs = 0;
};
struct OrderLimits {
    QString maxNotional, currency, maxDeviationPercent;
};
struct OrderValidation {
    bool ok = false;
    QString message, notional, currency;
};
enum class ExchangeAmendModel { Legacy, SameOrder, Unsupported };
struct ExchangePolicy {
    bool officialOrderTest = true;
    bool exactIntentPreflight = false;
    ExchangeAmendModel amendModel = ExchangeAmendModel::Legacy;
    QString marketBuyUnit;
};
struct TradingReadiness { bool ready = false; QString code, message; };
class ExchangeAdapter {
  public:
    virtual ~ExchangeAdapter() = default;
    virtual QString provider() const = 0;
    virtual ExchangePolicy exchangePolicy() const { return {}; }
    virtual QList<Operation> refreshPlan() const = 0;
    virtual RequestSpec build(Operation operation, const AccountRecord& account,
        const OrderInput& input, const CredentialSet& credentials, qint64 serverNowMs,
        QString* error) const = 0;
    virtual AdapterResult parse(Operation operation, const HttpResult& response,
        const OrderInput& input) const = 0;
    virtual OrderValidation validate(const OrderInput& input, const MarketSnapshot& snapshot,
        const OrderLimits& limits) const = 0;
    virtual QString amendDescription() const = 0;
};
} // namespace keefi
Q_DECLARE_METATYPE(keefi::Operation)
Q_DECLARE_METATYPE(keefi::HttpResult)
