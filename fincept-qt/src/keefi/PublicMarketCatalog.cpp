#include "keefi/PublicMarketCatalog.h"
#include "keefi/TradingTransport.h"
#include "keefi/TradingRequestLimits.h"
#include "keefi/PublicQuoteStream.h"
#include <QPointer>

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrlQuery>
#include <algorithm>
#include <utility>

namespace keefi {
namespace {
constexpr int PageSize = 100;        // Same page size as the existing public KRW price reads.
constexpr int RequestSpacingMs = 350; // Well below Upbit's 10 requests/second ticker group limit.
constexpr qint64 CatalogueMaxAgeMs = 300000;
constexpr qint64 MetadataRefreshLeadMs = 5000;
bool signedDecimal(const QString& value) {
    static const QRegularExpression re("\\A-?[0-9]{1,40}(?:\\.[0-9]{1,40})?(?:[eE][-+]?[0-9]{1,3})?\\z");
    return re.match(value).hasMatch();
}
// Upbit may send a rate in exponent form (e.g. 1.2E-4). Convert to plain digits without rounding.
QString plainDecimal(QString value) {
    const bool negative = value.startsWith('-');
    if (negative) value.remove(0, 1);
    const auto e = value.indexOf(QRegularExpression("[eE]"));
    if (e < 0) return (negative ? "-" : "") + value;
    const int exponent = value.mid(e + 1).toInt();
    QString mantissa = value.left(e);
    int dot = int(mantissa.indexOf('.'));
    if (dot < 0) dot = int(mantissa.size()); else mantissa.remove(dot, 1);
    int point = dot + exponent;
    if (point <= 0) { mantissa.prepend(QString(-point, '0')); point = 0; }
    if (point >= mantissa.size()) mantissa.append(QString(point - int(mantissa.size()), '0'));
    QString out = point == mantissa.size() ? mantissa : mantissa.left(point) + "." + mantissa.mid(point);
    if (out.startsWith('.')) out.prepend('0');
    return (negative ? "-" : "") + decimal::normalize(out);
}
} // namespace

QString changePercentText(const QString& signedRate) {
    if (!signedDecimal(signedRate)) return {};
    const auto plain = plainDecimal(signedRate);
    const bool negative = plain.startsWith('-');
    const auto magnitude = negative ? plain.mid(1) : plain;
    auto percent = decimal::multiply(magnitude, "100");
    if (percent.isEmpty()) return {};
    // Round half up to two decimals on the exact decimal string.
    const auto dot = percent.indexOf('.');
    QString whole = dot < 0 ? percent : percent.left(dot), fraction = dot < 0 ? QString{} : percent.mid(dot + 1);
    fraction = (fraction + "000").left(3);
    QString rounded = whole + "." + fraction.left(2);
    if (fraction[2] >= '5') rounded = decimal::add(rounded, "0.01");
    const auto parts = rounded.split('.');
    rounded = parts.value(0) + "." + (parts.value(1) + "00").left(2);
    const bool zero = rounded == "0.00";
    return (zero ? QString{} : negative ? QStringLiteral("-") : QStringLiteral("+")) + rounded + "%";
}

struct PublicMarketCatalog::Private {
    PublicMarketCatalog* q;
    TradingTransport* transport;
    QTimer timer;
    QTimer fallback;
    QPointer<PublicQuoteStream> stream;
    QElapsedTimer clock;
    Clock injectedClock;
    QList<PublicMarketRow> rows;
    QHash<QString, qsizetype> index;
    QSet<QString> marketCodes;
    quint64 quoteRevision = 1;
    QList<qsizetype> changedRows;
    QStringList remaining, requested;
    QString message, metadataError;
    quint64 token = 0;
    bool busy = false, catalogue = false, presentationActive = true, metadataDemand = false;
    qint64 catalogueAt = -CatalogueMaxAgeMs - 1, nextSend = 0, cooldown = 0, completedAt = 0;
    int updated = 0;
    Private(PublicMarketCatalog* owner, TradingTransport* t, Clock value) : q(owner), transport(t), injectedClock(std::move(value)) { clock.start(); timer.setSingleShot(true); }
    qint64 elapsed() const { return injectedClock ? injectedClock() : clock.elapsed(); }
    bool metadataFresh() const { const auto age=elapsed()-catalogueAt;return catalogueAt>=0&&age>=0&&age<=CatalogueMaxAgeMs; }
    bool metadataDue() const { return !metadataFresh()||elapsed()-catalogueAt>=CatalogueMaxAgeMs-MetadataRefreshLeadMs; }
    void syncFallback() { if(metadataDemand||(presentationActive&&stream))fallback.start();else fallback.stop(); }

    void recordQuotes(QList<qsizetype> positions) {
        if (positions.isEmpty()) return;
        // Match the full snapshot's catalogue order even for unordered or repeated ticks.
        std::sort(positions.begin(), positions.end());
        positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
        changedRows = std::move(positions);
        ++quoteRevision;
    }

    void finish(const QString& text) {
        busy = false; message = text;
        if(presentationActive)completedAt = QDateTime::currentMSecsSinceEpoch();
        emit q->changed();
    }
    void fail(const QString& reason) {
        if(catalogue)metadataError=reason;
        finish(updated ? QObject::tr("시세 부분 실패 · %1/%2종 갱신 · %3").arg(updated).arg(rows.size()).arg(reason)
                       : QObject::tr("시세 조회 실패 · %1").arg(reason));
    }
    void begin(bool forMetadata=false, bool retryMetadata=false) {
        if ((!presentationActive&&!metadataDemand) || busy) return;
        const bool needsCatalogue=retryMetadata||!metadataFresh()||(presentationActive&&rows.isEmpty())||(forMetadata&&metadataDemand&&metadataDue());
        if(!presentationActive&&!needsCatalogue)return;
        if (elapsed() < cooldown) {
            message = QObject::tr("업비트 요청 제한 대기 · %1초 후 다시 조회할 수 있습니다.").arg((cooldown - elapsed() + 999) / 1000);
            emit q->changed(); return;
        }
        busy = true; updated = 0;
        catalogue = needsCatalogue;
        if(catalogue)metadataError.clear();
        if (!catalogue) prepareQuotes();
        message = catalogue ? QObject::tr("업비트 KRW 종목 목록 조회 중") : QObject::tr("업비트 KRW 현재가 조회 중");
        QPointer<PublicMarketCatalog> alive(q);
        emit q->changed();
        if (alive) next();
    }
    void prepareQuotes() {
        remaining.clear();
        for (const auto& row : rows) remaining.append(row.market);
    }
    void next() {
        if ((!presentationActive&&(!metadataDemand||!catalogue)) || !busy) return;
        // Read the clock once: a second read can already be past nextSend, and Qt refuses a
        // negative interval, which left the catalogue busy for good.
        if (const auto wait = nextSend - elapsed(); wait > 0) { timer.start(int(wait)); return; }
        RequestSpec r;
        if (catalogue) {
            r.operation = Operation::Markets;
            r.url = QUrl("https://api.upbit.com/v1/market/all");
            QUrlQuery query; query.addQueryItem("is_details", "true"); r.url.setQuery(query);
        } else {
            if (remaining.isEmpty()) {
                finish(QObject::tr("업비트 KRW %1종 · 현재가·전일 대비 갱신").arg(rows.size()));
                return;
            }
            requested = remaining.mid(0, PageSize); remaining = remaining.mid(requested.size());
            r.operation = Operation::Quote;
            r.url = QUrl("https://api.upbit.com/v1/ticker");
            QUrlQuery query; query.addQueryItem("markets", requested.join(',')); r.url.setQuery(query);
        }
        nextSend = elapsed() + RequestSpacingMs;
        transport->send(++token, r);
    }
    void result(quint64 generation, const HttpResult& response) {
        if ((!presentationActive&&(!metadataDemand||!catalogue)) || generation != token || !busy) return;
        if (const auto wait = TradingRequestLimits::retryAfterMs(response); wait>0) {
            cooldown = elapsed() + wait;
            fail(QObject::tr("요청 제한 · %1초 후 재조회 가능").arg((wait+999)/1000)); return;
        }
        if (response.timedOut) { fail(QObject::tr("네트워크 시간 초과")); return; }
        if (response.networkError) { fail(QObject::tr("네트워크 연결 오류")); return; }
        if (response.status != 200) { fail(QObject::tr("서버 응답 오류 (HTTP %1)").arg(response.status)); return; }
        const auto document = parseExactJson(response.body);
        if (!document.isArray()) { fail(QObject::tr("응답 JSON 형식 오류")); return; }
        if (catalogue) { acceptCatalogue(document.array()); return; }
        acceptQuotes(document.array());
    }
    void acceptCatalogue(const QJsonArray& array) {
        static const QRegularExpression krw("\\AKRW-([A-Z][A-Z0-9]{0,23})\\z");
        if (array.size() > 20000) { fail(QObject::tr("공식 종목 목록 형식 오류")); return; }
        QList<PublicMarketRow> next; QSet<QString> seen;
        for (const auto& value : array) {
            const auto o = value.toObject();
            const auto market = o.value("market").toString();
            if (!value.isObject() || market.isEmpty() || seen.contains(market) || !o.value("korean_name").isString()) {
                fail(QObject::tr("공식 종목 목록 형식 오류")); return;
            }
            seen.insert(market);
            const auto match = krw.match(market);
            if (!match.hasMatch()) continue;
            PublicMarketRow row;
            row.market = market; row.base = match.captured(1);
            row.koreanName = o.value("korean_name").toString().left(80);
            row.englishName = o.value("english_name").toString().left(80);
            const auto event = o.value("market_event").toObject();
            row.warning = event.value("warning").toBool();
            const auto caution = event.value("caution").toObject();
            for (auto it = caution.constBegin(); it != caution.constEnd(); ++it) row.caution = row.caution || it.value().toBool();
            // Keep the previous price of a still-listed market until a new quote replaces it.
            if (index.contains(market)) {
                const auto& old = rows[index.value(market)];
                row.priced = old.priced; row.price = old.price; row.previousClose = old.previousClose;
                row.signedChangeRate = old.signedChangeRate; row.sourceTimeMs = old.sourceTimeMs; row.receivedAtMs = old.receivedAtMs;
                row.quoteUrl = old.quoteUrl; row.tradeValue24h = old.tradeValue24h; row.tradeVolume24h = old.tradeVolume24h;
            }
            next.append(row);
        }
        rows = std::move(next); index.clear(); marketCodes.clear();
        for (qsizetype i = 0; i < rows.size(); ++i) {
            const auto& market = rows.at(i).market;
            index.insert(market, i);
            marketCodes.insert(market);
        }
        changedRows.clear(); // A replaced catalogue invalidates the previous row positions.
        ++quoteRevision;
        catalogueAt = elapsed(); catalogue = false; metadataError.clear();
        QPointer<PublicMarketCatalog> alive(q);
        if(stream&&presentationActive){QStringList codes;for(const auto& row:rows)codes.append(row.market);stream->setMarkets(codes);}
        if (!alive) return;
        if(!presentationActive){busy=false;message=QObject::tr("탐색 대기 · 공개 종목 목록 확인");emit q->changed();return;}
        prepareQuotes(); emit q->changed(); if (alive) next_();
    }
    void next_() { if (presentationActive && busy) timer.start(int(qMax<qint64>(0, nextSend - elapsed()))); }
    void acceptQuotes(const QJsonArray& array) {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        QHash<QString, PublicMarketRow> page; const QSet<QString> wanted(requested.cbegin(), requested.cend());
        for (const auto& value : array) {
            const auto o = value.toObject();
            const auto market = o.value("market").toString();
            const auto price = o.value("trade_price").toString(), previous = o.value("prev_closing_price").toString();
            const auto rate = o.value("signed_change_rate").toString();
            bool ok = false; const auto time = o.value("timestamp").toString().toLongLong(&ok);
            if (!value.isObject() || !wanted.contains(market) || page.contains(market) || !decimal::positive(price) ||
                !decimal::positive(previous) || !signedDecimal(rate) || !ok || time <= 0 || time - now > 2000) {
                fail(QObject::tr("현재가 응답 범위 오류")); return;
            }
            PublicMarketRow row; row.price = price; row.previousClose = previous; row.signedChangeRate = plainDecimal(rate);
            row.sourceTimeMs = time; row.receivedAtMs = now; row.priced = true;
            { const auto value = o.value("acc_trade_price_24h").toString(); if (decimal::positive(value)) row.tradeValue24h = value; }
            { const auto value = o.value("acc_trade_volume_24h").toString(); if (decimal::positive(value)) row.tradeVolume24h = value; }
            page.insert(market, row);
        }
        if (page.size() != wanted.size()) { fail(QObject::tr("요청한 현재가 일부 누락")); return; }
        // Commit only a completely validated page.
        QList<qsizetype> positions;
        for (auto it = page.cbegin(); it != page.cend(); ++it) {
            const auto pos = index.value(it.key());
            auto& row = rows[pos];
            if(row.priced&&it->sourceTimeMs<=row.sourceTimeMs)continue;
            positions.append(pos);
            row.priced = true; row.price = it->price; row.previousClose = it->previousClose; row.signedChangeRate = it->signedChangeRate;
            if (!it->tradeValue24h.isEmpty()) row.tradeValue24h = it->tradeValue24h;
            row.tradeVolume24h = it->tradeVolume24h;
            row.sourceTimeMs = it->sourceTimeMs; row.receivedAtMs = it->receivedAtMs;
            row.quoteUrl="https://api.upbit.com/v1/ticker";
        }
        recordQuotes(std::move(positions));
        updated += int(page.size());
        QPointer<PublicMarketCatalog> alive(q);
        emit q->changed();
        if (alive) next_();
    }
};

PublicMarketCatalog::PublicMarketCatalog(TradingTransport* transport, QObject* parent, Clock clock)
    : QObject(parent), d_(std::make_unique<Private>(this, transport, std::move(clock))) {
    connect(&d_->timer, &QTimer::timeout, this, [this] { d_->next(); });
    d_->fallback.setInterval(5000);
    connect(&d_->fallback,&QTimer::timeout,this,[this]{
        if(d_->busy)return;
        if(d_->metadataDemand&&d_->metadataDue())d_->begin(true);
        else if(d_->presentationActive&&d_->stream&&
            (QDateTime::currentMSecsSinceEpoch()-d_->completedAt>5000||!d_->metadataFresh()))d_->begin();
    });
    connect(transport, &TradingTransport::finished, this, [this](quint64 token, const HttpResult& r) { d_->result(token, r); });
}
PublicMarketCatalog::~PublicMarketCatalog() { ++d_->token; d_->timer.stop(); }
void PublicMarketCatalog::refresh() { d_->begin(); }
void PublicMarketCatalog::retryMarketList() {
    if(marketListState()==MarketListState::Failed)d_->begin(false,true);
}
void PublicMarketCatalog::setMetadataDemand(bool active) {
    if(d_->metadataDemand==active)return;d_->metadataDemand=active;d_->syncFallback();
    if(active){if(d_->metadataDue())d_->begin(true);return;}
    if(!d_->presentationActive&&d_->busy){
        ++d_->token;d_->busy=false;d_->timer.stop();d_->remaining.clear();d_->requested.clear();
        QPointer<PublicMarketCatalog> alive(this);d_->transport->abort();if(alive)emit changed();
    }
}
void PublicMarketCatalog::setPresentationActive(bool active) {
    if (d_->presentationActive == active) return;
    d_->presentationActive = active;
    QPointer<PublicMarketCatalog> alive(this);
    if (!active) {
        // Revoke acceptance before abort: transports may synchronously emit finished.
        const bool retainMetadata=d_->metadataDemand&&d_->catalogue&&d_->busy;
        const bool wasBusy = d_->busy&&!retainMetadata;
        if(!retainMetadata){++d_->token;d_->busy=false;d_->timer.stop();}
        d_->syncFallback();
        d_->remaining.clear(); d_->requested.clear();
        d_->message = tr("탐색 대기 · 마지막 공개 시세 시각 유지");
        if (wasBusy) d_->transport->abort();
        if (!alive) return;
        if (d_->stream) d_->stream->setMarkets({});
        if (!alive) return;
        emit changed();
        if(alive&&d_->metadataDemand&&!retainMetadata)d_->begin(true);
        return;
    }
    if (d_->stream) {
        QStringList codes; for (const auto& row : d_->rows) codes.append(row.market);
        d_->stream->setMarkets(codes);
        if (!alive || !d_->presentationActive) return;
    }
    d_->syncFallback();
    // The elapsed pacing, metadata cache and source cooldown survive every toggle.
    d_->begin();
}
bool PublicMarketCatalog::presentationActive() const { return d_->presentationActive; }
void PublicMarketCatalog::setStream(PublicQuoteStream* stream) {
    if(d_->stream){d_->stream->setMarkets({});disconnect(d_->stream,nullptr,this,nullptr);}
    d_->stream=stream;d_->syncFallback();if(!stream)return;
    connect(stream,&PublicQuoteStream::ticks,this,[this](const QList<PublicTick>& ticks){
        if (!d_->presentationActive) return;
        QList<qsizetype> positions;
        for(const auto& tick:ticks){
            const auto pos = d_->index.constFind(tick.market);
            if(pos==d_->index.cend())continue;
            auto& row=d_->rows[pos.value()];
            if(row.priced&&tick.quote.evidence.sourceTimeMs<=row.sourceTimeMs)continue;
            positions.append(pos.value());
            row.price=tick.quote.price;row.previousClose=tick.previousClose;row.signedChangeRate=plainDecimal(tick.signedChangeRate);
            row.sourceTimeMs=tick.quote.evidence.sourceTimeMs;row.receivedAtMs=tick.quote.evidence.receivedAtMs;row.priced=true;
            if(!tick.tradeValue24h.isEmpty())row.tradeValue24h=tick.tradeValue24h;
            row.quoteUrl=tick.quote.evidence.url;
        }
        if(!positions.isEmpty()){
            d_->recordQuotes(std::move(positions));
            d_->completedAt=QDateTime::currentMSecsSinceEpoch();emit changed();
        }
    });
    connect(stream,&PublicQuoteStream::changed,this,&PublicMarketCatalog::changed);
    QStringList codes;if(d_->presentationActive)for(const auto& row:d_->rows)codes.append(row.market);stream->setMarkets(codes);
}
namespace {
AssetQuote marketQuote(const PublicMarketRow& row) {
    return {row.base, "KRW", row.price, {"업비트 공식 KRW 현재가",
        row.quoteUrl.isEmpty() ? QStringLiteral("https://api.upbit.com/v1/ticker") : row.quoteUrl,
        "public:upbit:" + row.market, row.sourceTimeMs, row.receivedAtMs, true}};
}
}
QList<AssetQuote> PublicMarketCatalog::quotes() const {
    QList<AssetQuote> quotes;
    for (const auto& row : d_->rows) if (row.priced) quotes.append(marketQuote(row));
    return quotes;
}
QList<AssetQuote> PublicMarketCatalog::quotesSince(quint64& revision) const {
    if (revision == d_->quoteRevision) return {};
    const bool hasDelta = revision != 0 && revision + 1 == d_->quoteRevision && !d_->changedRows.isEmpty();
    revision = d_->quoteRevision;
    if (!hasDelta) return quotes();
    QList<AssetQuote> updates;
    updates.reserve(d_->changedRows.size());
    for (const auto pos : std::as_const(d_->changedRows)) updates.append(marketQuote(d_->rows.at(pos)));
    return updates;
}
QSet<QString> PublicMarketCatalog::marketCodes() const { return d_->marketCodes; }
AssetQuote PublicMarketCatalog::quoteForMarket(const QString& market) const {
    const auto it = d_->index.constFind(market);
    if (it == d_->index.cend()) return {};
    const auto& row = d_->rows.at(it.value());
    return row.priced ? marketQuote(row) : AssetQuote{};
}
bool PublicMarketCatalog::busy() const { return d_->busy; }
QList<PublicMarketRow> PublicMarketCatalog::rows() const { return d_->rows; }
PublicMarketRow PublicMarketCatalog::rowForMarket(const QString& market) const {
    const auto pos = d_->index.constFind(market);
    return pos == d_->index.cend() ? PublicMarketRow{} : d_->rows.at(pos.value());
}
bool PublicMarketCatalog::marketListAvailable() const {
    return d_->metadataFresh();
}
PublicMarketCatalog::MarketListState PublicMarketCatalog::marketListState() const {
    if(d_->busy&&d_->catalogue)return MarketListState::Loading;
    if(!d_->metadataError.isEmpty())return MarketListState::Failed;
    return d_->catalogueAt>=0?MarketListState::Ready:MarketListState::NotRequested;
}
QString PublicMarketCatalog::marketListError() const { return d_->metadataError; }
qint64 PublicMarketCatalog::retryAfterMs() const { return qMax<qint64>(0,d_->cooldown-d_->elapsed()); }
QStringList PublicMarketCatalog::verifiedMarkets() const {
    QStringList result;if(marketListAvailable())for(const auto& row:d_->rows)result.append(row.market);return result;
}
QString PublicMarketCatalog::status() const { return d_->stream&&!d_->stream->status().isEmpty()?d_->stream->status()+" · "+d_->message:d_->message; }
qint64 PublicMarketCatalog::completedAtMs() const { return d_->completedAt; }

} // namespace keefi
