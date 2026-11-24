#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/BinanceAdapter.h"
#include "keefi/BithumbAdapter.h"
#include "keefi/ExchangeIdentifiers.h"
#include "keefi/TradingTransport.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QPointer>
#include <QQueue>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrlQuery>
#include <exception>

namespace keefi {
namespace {
bool supported(const QString& provider) { return provider == "binance" || provider == "bithumb"; }
QString source(const QString& provider) {
    if (provider == "binance") return "https://api.binance.com/api/v3/exchangeInfo";
    if (provider == "bithumb") return "https://api.bithumb.com/v1/market/all";
    return {};
}
std::unique_ptr<ExchangeAdapter> codec(const QString& provider) {
    if (provider == "binance") return std::make_unique<BinanceAdapter>();
    if (provider == "bithumb") return std::make_unique<BithumbAdapter>();
    return {};
}
bool publicRequest(const QString& provider, const RequestSpec& request) {
    if (request.operation != Operation::Markets || request.method != "GET" || request.authenticated || !request.body.isEmpty() ||
        request.url.scheme() != "https" || !request.url.userInfo().isEmpty() || request.url.port() != -1 || request.url.hasFragment()) return false;
    const auto query = QUrlQuery(request.url).queryItems();
    if (provider == "binance") return request.url.host() == "api.binance.com" && request.url.path() == "/api/v3/exchangeInfo" &&
        request.headers.isEmpty() && query.size() == 2 && query.contains({"permissions", "SPOT"}) && query.contains({"showPermissionSets", "false"});
    return provider == "bithumb" && request.url.host() == "api.bithumb.com" && request.url.path() == "/v1/market/all" &&
        request.headers.size() == 1 && request.headers.value("Accept") == "application/json" &&
        query.size() == 1 && query.first() == qMakePair(QString("isDetails"), QString("true"));
}
// Adapters normalize wire data. The catalogue projection additionally checks
// complete, unambiguous identity tuples before publishing any row.
bool project(const QString& provider, const QJsonObject& data, QList<MarketInstrument>* output) {
    const auto values = data.value(provider == "binance" ? "symbols" : "instruments");
    if (!values.isArray() || values.toArray().isEmpty() || values.toArray().size() > 20000) return false;
    QList<MarketInstrument> next;
    QSet<QString> seen;
    static const QRegularExpression statePattern("\\A[A-Z_]{1,40}\\z");
    static const QRegularExpression bithumbPattern("\\A(?:KRW|BTC|USDT)-[A-Z0-9]{1,40}\\z");
    for (const auto& value : values.toArray()) {
        if (!value.isObject()) return false;
        const auto row = value.toObject();
        MarketInstrument item;
        item.market = "spot";
        item.symbol = row.value("symbol").toString();
        item.baseAsset = row.value("baseAsset").toString();
        item.quoteAsset = row.value("quoteAsset").toString();
        if (seen.contains(item.symbol)) return false;
        seen.insert(item.symbol);
        if (provider == "binance") {
            const auto state = row.value("status").toString();
            if (!binanceIdentifierValid(item.symbol, 2) || !binanceIdentifierValid(item.baseAsset) || !binanceIdentifierValid(item.quoteAsset) ||
                item.symbol != item.baseAsset + item.quoteAsset || !statePattern.match(state).hasMatch() || !row.value("isSpotTradingAllowed").isBool()) return false;
            item.tradable = state == "TRADING" && row.value("isSpotTradingAllowed").toBool();
            item.status = state;
        } else {
            if (!bithumbPattern.match(item.symbol).hasMatch() || item.symbol != item.quoteAsset + "-" + item.baseAsset || !row.value("tradable").isBool()) return false;
            item.name = row.value("name").toString(); item.englishName = row.value("englishName").toString();
            item.warning = row.value("warning").toString(); item.status = row.value("status").toString();
            item.tradable = row.value("tradable").toBool();
        }
        next.append(item);
    }
    *output = next;
    return true;
}
qint64 retryDelay(const HttpResult& response, qint64 now) {
    qint64 milliseconds = ChartInstrumentCatalog::FreshnessMs;
    if (response.localRetryAfterSeconds > 0) milliseconds = qMax(milliseconds, qint64(response.localRetryAfterSeconds) * 1000);
    for (auto it = response.headers.cbegin(); it != response.headers.cend(); ++it) {
        if (it.key().compare("Retry-After", Qt::CaseInsensitive) != 0) continue;
        bool ok = false;
        const auto seconds = it.value().toLongLong(&ok);
        if (ok && seconds > 0 && seconds <= 999999999) milliseconds = qMax(milliseconds, seconds * 1000);
        else {
            const auto date = QDateTime::fromString(QString::fromLatin1(it.value()), Qt::RFC2822Date);
            if (date.isValid() && date.toMSecsSinceEpoch() > now) milliseconds = qMax(milliseconds, date.toMSecsSinceEpoch() - now);
        }
    }
    return milliseconds;
}
} // namespace

struct ChartInstrumentCatalog::Private {
    struct Cache {
        QList<MarketInstrument> rows;
        qint64 receivedAtMs = 0, retryUntil = 0;
        QString message;
        bool failed = false;
    };
    ChartInstrumentCatalog* q;
    QPointer<TradingTransport> transport;
    QHash<QString, Cache> caches;
    QQueue<QString> queue;
    QElapsedTimer clock;
    Clock injectedClock;
    QTimer pacing, timeout;
    QString activeProvider;
    quint64 activeToken = 0, nextToken = 1;
    qint64 nextSendAt = 0;
    Private(ChartInstrumentCatalog* owner, TradingTransport* lane, Clock value) : q(owner), transport(lane), injectedClock(std::move(value)) {
        clock.start(); pacing.setSingleShot(true); timeout.setSingleShot(true); timeout.setInterval(10000);
    }
    qint64 elapsed() const { return injectedClock?injectedClock():clock.elapsed(); }
    bool fresh(const Cache& cache, qint64 at) const {
        return !cache.rows.isEmpty() && cache.receivedAtMs > 0 && at >= cache.receivedAtMs && at - cache.receivedAtMs <= FreshnessMs;
    }
    void next() {
        if (activeToken || queue.isEmpty() || !transport) return;
        if (const auto wait = nextSendAt - elapsed(); wait > 0) { pacing.start(int(wait)); return; } // One clock read; never a negative wait.
        const auto provider = queue.dequeue();
        auto& cache = caches[provider];
        const auto now = QDateTime::currentMSecsSinceEpoch();
        if (fresh(cache, now) || elapsed() < cache.retryUntil) { next(); return; }
        cache.retryUntil = elapsed() + FreshnessMs;
        QString error;
        RequestSpec request;
        try {
            AccountRecord account; account.provider = provider; account.market = "spot"; account.environment = "live";
            const auto adapter = codec(provider);
            if (adapter) request = adapter->build(Operation::Markets, account, {}, CredentialSet{}, now, &error);
        } catch (const std::exception&) { error = "public codec failure"; }
        if (!error.isEmpty() || !publicRequest(provider, request)) {
            cache.rows.clear(); cache.receivedAtMs = 0; cache.failed = true; cache.message = QObject::tr("공개 종목 요청 검증 실패");
            emit q->changed(); next(); return;
        }
        activeProvider = provider; activeToken = nextToken++;
        nextSendAt = elapsed() + 350;
        cache.failed = false;
        cache.message = QObject::tr("공개 종목 목록 조회 중");
        timeout.start();
        transport->send(activeToken, request);
        emit q->changed();
    }
    void result(quint64 token, const HttpResult& response) {
        if (!token || token != activeToken) return;
        timeout.stop();
        const auto provider = activeProvider;
        activeToken = 0; activeProvider.clear();
        complete(provider, response);
    }
    void complete(const QString& provider, const HttpResult& response) {
        auto& cache = caches[provider];
        cache.rows.clear(); cache.receivedAtMs = 0;
        const auto now = QDateTime::currentMSecsSinceEpoch();
        cache.retryUntil = qMax(cache.retryUntil, elapsed() + retryDelay(response, now));
        QList<MarketInstrument> instruments;
        bool valid = false;
        if (!response.timedOut && !response.networkError && response.status == 200 && response.localRetryAfterSeconds <= 0) {
            try {
                const auto adapter = codec(provider);
                const auto decoded = adapter->parse(Operation::Markets, response, {});
                valid = decoded.ok && project(provider, decoded.data, &instruments);
            } catch (const std::exception&) {}
        }
        cache.failed = !valid;
        if (valid) {
            cache.rows = instruments; cache.receivedAtMs = now;
            cache.message = QObject::tr("공개 종목 %1개 확인").arg(instruments.size());
        } else if (response.status == 429 || response.status == 418 || response.localRetryAfterSeconds > 0) cache.message = QObject::tr("공개 원천 요청 제한 · 재조회 대기");
        else if (response.timedOut) cache.message = QObject::tr("공개 원천 응답 시간 초과");
        else if (response.networkError) cache.message = QObject::tr("공개 원천 연결 오류");
        else if(response.status>0&&response.status!=200)cache.message=QObject::tr("서버 응답 오류 (HTTP %1)").arg(response.status);
        else cache.message = QObject::tr("공개 종목 원천 응답 검증 실패");
        emit q->changed();
        next();
    }
};

ChartInstrumentCatalog::ChartInstrumentCatalog(TradingTransport* transport, QObject* parent, Clock clock)
    : QObject(parent), d_(std::make_unique<Private>(this, transport, std::move(clock))) {
    setObjectName("chartInstrumentCatalog");
    connect(&d_->pacing, &QTimer::timeout, this, [this] { d_->next(); });
    connect(&d_->timeout, &QTimer::timeout, this, [this] {
        if (!d_->activeToken) return;
        const auto provider = d_->activeProvider;
        HttpResult response; response.timedOut = true;
        // Revoke first: abort() may synchronously deliver the canceled reply.
        d_->activeToken = 0; d_->activeProvider.clear();
        if (d_->transport) d_->transport->abort();
        d_->complete(provider, response);
    });
    if (transport) {
        connect(transport, &TradingTransport::finished, this, [this](quint64 token, const HttpResult& response) { d_->result(token, response); });
        connect(transport, &QObject::destroyed, this, [this] {
            const auto provider = d_->activeProvider;
            d_->transport = nullptr; d_->activeToken = 0; d_->activeProvider.clear(); d_->queue.clear();
            d_->pacing.stop(); d_->timeout.stop();
            if (!provider.isEmpty()) { auto& cache = d_->caches[provider]; cache.rows.clear(); cache.receivedAtMs = 0; cache.failed = true; cache.message = tr("공개 원천 연결 종료"); }
            emit changed();
        });
    }
}
ChartInstrumentCatalog::~ChartInstrumentCatalog() {
    if (d_->transport) disconnect(d_->transport, nullptr, this, nullptr);
    d_->pacing.stop(); d_->timeout.stop();
    if (d_->activeToken && d_->transport) { d_->activeToken = 0; d_->transport->abort(); }
}
void ChartInstrumentCatalog::ensure(const QString& provider) {
    if (!supported(provider) || !d_->transport) return;
    auto& cache = d_->caches[provider];
    if (d_->fresh(cache, QDateTime::currentMSecsSinceEpoch()) || d_->elapsed() < cache.retryUntil ||
        d_->activeProvider == provider || d_->queue.contains(provider)) return;
    d_->queue.enqueue(provider);
    d_->next();
}
QList<MarketInstrument> ChartInstrumentCatalog::instruments(const QString& provider, qint64 asOfMs) const {
    const auto found = d_->caches.constFind(provider);
    if (!supported(provider) || found == d_->caches.cend()) return {};
    const auto at = asOfMs > 0 ? asOfMs : QDateTime::currentMSecsSinceEpoch();
    return d_->fresh(found.value(), at) ? found->rows : QList<MarketInstrument>{};
}
QString ChartInstrumentCatalog::status(const QString& provider) const {
    if (!supported(provider)) return tr("지원하지 않는 공개 종목 원천");
    const auto cache = d_->caches.value(provider);
    QString text = source(provider) + " · " + (cache.message.isEmpty() ? tr("공개 종목 확인 대기") : cache.message);
    if (cache.receivedAtMs > 0) text += tr(" · 수신 ") + QDateTime::fromMSecsSinceEpoch(cache.receivedAtMs).toString("yyyy-MM-dd HH:mm:ss t") +
        (d_->fresh(cache, QDateTime::currentMSecsSinceEpoch()) ? tr(" · 유효 (5분)") : tr(" · 만료"));
    return text;
}
bool ChartInstrumentCatalog::busy() const { return d_->activeToken || !d_->queue.isEmpty(); }
QString ChartInstrumentCatalog::failureReason(const QString& provider) const {
    const auto it=d_->caches.constFind(provider);
    return it!=d_->caches.cend()&&it->failed?it->message:QString{};
}
qint64 ChartInstrumentCatalog::retryAfterMs(const QString& provider) const {
    return qMax<qint64>(0,d_->caches.value(provider).retryUntil-d_->elapsed());
}
bool ChartInstrumentCatalog::canRetry(const QString& provider) const {
    return d_->transport && !failureReason(provider).isEmpty() && retryAfterMs(provider)==0;
}
void ChartInstrumentCatalog::stop() {
    const auto token = d_->activeToken;
    const auto provider = d_->activeProvider;
    d_->activeToken = 0; d_->activeProvider.clear(); d_->queue.clear(); d_->pacing.stop(); d_->timeout.stop();
    if (token && d_->transport) d_->transport->abort();
    if (!provider.isEmpty()) d_->caches[provider].message = tr("공개 종목 조회 중단");
    emit changed();
}
} // namespace keefi
