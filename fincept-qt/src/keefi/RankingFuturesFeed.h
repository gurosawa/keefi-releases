#pragma once
#include "keefi/RankingStudies.h"
#include "keefi/TradingTypes.h"
#include <QObject>
#include <functional>
#include <memory>

namespace keefi {
class TradingTransport;
class ChartInstrumentCatalog;
class ChartStudyRequestPacer;
// Visible ranking rows only. Public fixed-request display observations; no
// account inputs, CVD streams, subscription expansion or valuation effects.
class RankingFuturesFeed final:public QObject {
    Q_OBJECT
public:
    using Clock=std::function<qint64()>;
    static constexpr int MaximumTargets=20;
    static constexpr qint64 FreshnessMs=180000;
    explicit RankingFuturesFeed(TradingTransport*,QObject* parent=nullptr,Clock={});
    ~RankingFuturesFeed() override;
    void setCatalog(ChartInstrumentCatalog*);
    void setRequestPacer(std::shared_ptr<ChartStudyRequestPacer>);
    void setTargets(const QStringList& baseAssets,RankingStudy);
    void clear();
    void pump();
    RankingStudyValue observation(const QString& baseAsset) const;
    // Parsing seams assume the caller separately verified exact spot and
    // USDT perpetual identities. The feed enforces both catalogues before HTTP.
    static RankingStudyValue parseOpenInterest24h(const QString& base,const HttpResult&,qint64 asOfMs);
    static RankingStudyValue parseSpotFutures(const QString& base,const HttpResult& spot,const HttpResult& futures,qint64 asOfMs);
signals:
    void changed();
private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}
