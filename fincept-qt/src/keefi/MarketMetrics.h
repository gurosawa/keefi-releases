#pragma once
#include "keefi/AssetValuation.h"
#include "keefi/TradingTypes.h"
#include <QObject>
#include <memory>

namespace keefi {
class TradingTransport;
struct ExchangePriceGap {
    bool available = false;
    QString percent, reason; // Signed %, truncated toward zero to six decimal places.
    QString label = QStringLiteral("USDT 기준 거래소 가격차");
    QList<AssetQuote> legs; // Local KRW or USD/asset, USDT/asset, matching local/USDT.
    qint64 asOfMs = 0;
};
struct PublicDerivativesMetrics {
    QString symbol, baseAsset;
    QString fundingRate, interestRate; // Published decimal ratios, not settled payments.
    int fundingIntervalHours = 0; // Zero means unknown, never an implicit eight hours.
    bool fundingIntervalDefault = false;
    qint64 nextFundingTimeMs = 0;
    QString openInterest, openInterestUnit;
    AssetEvidence fundingEvidence, intervalEvidence, openInterestEvidence;
    QString fundingReason, intervalReason, openInterestReason;
    QString markPrice, flowLabel, flowReason;
    qint64 flowFromMs=0, flowToMs=0;
    bool flowAvailable=false;
    int flowPriceDirection=0, flowQuantityDirection=0;
};
struct ContractObservation { QString symbol,price,quantity; qint64 priceAtMs=0,quantityAtMs=0; };
struct ContractFlow { bool available=false; int priceDirection=0,quantityDirection=0; QString label,reason; qint64 fromMs=0,toMs=0; };
// Public display context only. These values never enter portfolio valuation.
class MarketMetrics final : public QObject {
    Q_OBJECT
  public:
    enum class Read { Contracts, Funding, FundingIntervals, OpenInterest };
    static constexpr qint64 FreshnessMs = 180000;
    static constexpr qint64 MaximumLegSkewMs = 30000;
    explicit MarketMetrics(TradingTransport*, QObject* parent = nullptr);
    ~MarketMetrics() override;
    void setAsset(const QString& asset); // Empty stops requests; only one visible coin is fetched.
    static QString pendingReason() { return QStringLiteral("조회 대기"); } // Before the first answer for the asset.
    PublicDerivativesMetrics derivatives(qint64 asOfMs = 0) const;
    static ExchangePriceGap exchangePriceGap(const AssetQuote& upbit,
        const AssetQuote& binance, const AssetQuote& krwUsdt, qint64 asOfMs);
    // Arithmetic only; callers retain their own original source/time validation.
    static QString priceGapPercent(const QString& localPrice, const QString& remotePrice, const QString& bridgePrice);
    static RequestSpec request(Read, const QString& symbol = {});
    static bool isRequest(const RequestSpec&);
    // Each response is independent: a failed funding read cannot erase valid OI.
    static bool applyResponse(Read, const QString& symbol, const HttpResult&,
        qint64 receivedAtMs, PublicDerivativesMetrics*);
    static ContractFlow contractFlow(const ContractObservation&,const ContractObservation&,qint64 asOfMs);
  signals:
    void changed();
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
} // namespace keefi
