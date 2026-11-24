#include "keefi/PublicEquityBoard.h"
#include "keefi/TradingTransport.h"
#include "keefi/TradingTypes.h"
#include "keefi/TradingRequestLimits.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>
#include <QUrlQuery>
#include <algorithm>
#include <cmath>

namespace keefi {
namespace {
constexpr int CoinLimit = 200, KoreaLimit = 50, UsLimit = 80;
constexpr qint64 UsRefreshMs = 60 * 1000;

const QHash<QString, QString>& koreaSectors() {
    static const QHash<QString, QString> sectors{
        {"005930", "반도체·전자"}, {"005935", "반도체·전자"}, {"000660", "반도체·전자"}, {"009150", "반도체·전자"}, {"066570", "반도체·전자"},
        {"034220", "반도체·전자"}, {"011070", "반도체·전자"}, {"042700", "반도체·전자"},
        {"373220", "2차전지·화학"}, {"006400", "2차전지·화학"}, {"051910", "2차전지·화학"}, {"003670", "2차전지·화학"}, {"096770", "2차전지·화학"},
        {"010950", "2차전지·화학"}, {"011170", "2차전지·화학"},
        {"005380", "자동차"}, {"000270", "자동차"}, {"012330", "자동차"}, {"005387", "자동차"}, {"161390", "자동차"},
        {"207940", "바이오"}, {"068270", "바이오"}, {"000100", "바이오"}, {"128940", "바이오"}, {"326030", "바이오"},
        {"105560", "금융"}, {"055550", "금융"}, {"086790", "금융"}, {"316140", "금융"}, {"032830", "금융"}, {"000810", "금융"},
        {"138040", "금융"}, {"024110", "금융"}, {"323410", "금융"}, {"006800", "금융"}, {"071050", "금융"},
        {"012450", "조선·방산·기계"}, {"034020", "조선·방산·기계"}, {"329180", "조선·방산·기계"}, {"009540", "조선·방산·기계"},
        {"042660", "조선·방산·기계"}, {"010140", "조선·방산·기계"}, {"267260", "조선·방산·기계"}, {"064350", "조선·방산·기계"},
        {"047810", "조선·방산·기계"}, {"010120", "조선·방산·기계"},
        {"028260", "지주"}, {"000880", "지주"}, {"003550", "지주"}, {"034730", "지주"}, {"402340", "지주"}, {"267250", "지주"},
        {"035420", "인터넷·게임"}, {"035720", "인터넷·게임"}, {"259960", "인터넷·게임"}, {"036570", "인터넷·게임"}, {"018260", "인터넷·게임"},
        {"005490", "철강·소재"}, {"010130", "철강·소재"},
        {"017670", "통신"}, {"030200", "통신"}, {"032640", "통신"},
        {"015760", "유틸리티"}, {"036460", "유틸리티"},
        {"033780", "소비재"}, {"090430", "소비재"}, {"097950", "소비재"}, {"271560", "소비재"},
        {"011200", "운송"}, {"003490", "운송"}, {"086280", "운송"}};
    return sectors;
}
QString usSector(const QString& sector) {
    static const QHash<QString, QString> names{{"Technology", "기술"}, {"Finance", "금융"}, {"Health Care", "헬스케어"},
        {"Consumer Discretionary", "경기소비재"}, {"Industrials", "산업재"}, {"Energy", "에너지"}, {"Consumer Staples", "필수소비재"},
        {"Telecommunications", "통신"}, {"Utilities", "유틸리티"}, {"Real Estate", "부동산"}, {"Basic Materials", "소재"}};
    return names.value(sector, QStringLiteral("기타"));
}
QString coinGroup(const QString& symbol) {
    static const QSet<QString> major{"BTC", "ETH"}, meme{"DOGE", "SHIB", "PEPE", "BONK", "WIF", "TRUMP", "PENGU", "FLOKI"},
        defi{"LINK", "UNI", "AAVE", "ONDO", "ENA", "JUP", "MKR", "LDO"}, payment{"XRP", "XLM", "LTC", "BCH", "XMR", "ZEC", "DASH"};
    if (major.contains(symbol)) return QStringLiteral("주요");
    if (meme.contains(symbol)) return QStringLiteral("밈");
    if (defi.contains(symbol)) return QStringLiteral("디파이");
    if (payment.contains(symbol)) return QStringLiteral("결제");
    return QStringLiteral("레이어1·기타");
}
QString number(double value, int decimals) { return QString::number(value, 'f', decimals); }
QString signedPercent(double value) { return QString::number(value, 'f', 2); }
QString plain(const QString& grouped) { QString out = grouped; out.remove(','); return out.trimmed(); }
bool decimalText(const QString& text) {
    static const QRegularExpression re(QStringLiteral("\\A-?[0-9]{1,20}(?:\\.[0-9]{1,12})?\\z"));
    return re.match(text).hasMatch();
}
} // namespace

struct PublicEquityBoard::Private {
    PublicEquityBoard* q = nullptr;
    QPointer<TradingTransport> transport, rankingTransport;
    bool separateRanking = false;
    QHash<QString, QList<PublicBoardItem>> items;
    QHash<QString, PublicBoardItem> known;
    QHash<QString, qint64> received;
    QHash<QString, QString> status, market;
    QStringList queue;
    quint64 token = 0, rankingToken = 0, nextToken = 1;
    QString current;
    qint64 nextBinanceMs=0;
    void next() {
        if (token || queue.isEmpty() || !transport) return;
        current = queue.takeFirst();
        token = nextToken++;
        if(current=="binance")nextBinanceMs=QDateTime::currentMSecsSinceEpoch()+UsRefreshMs;
        transport->send(token, PublicEquityBoard::request(current));
    }
    void finished(quint64 reply, const HttpResult& response) {
        if (!token || reply != token) return;
        const auto section = current;
        token = 0;
        current.clear();
        QPointer<PublicEquityBoard> owner = q;
        apply(section, response);
        if (owner) next();
    }
    void rankingFinished(quint64 reply, const HttpResult& response) {
        if (!rankingToken || reply != rankingToken) return;
        rankingToken = 0;
        apply(QStringLiteral("binance"), response);
    }
    void apply(const QString& section, const HttpResult& response) {
        QList<PublicBoardItem> parsed; QString marketStatus;
        if(section=="binance"){
            const auto wait=TradingRequestLimits::retryAfterMs(response);
            if(wait>0)nextBinanceMs=QDateTime::currentMSecsSinceEpoch()+qMax<qint64>(UsRefreshMs,wait);
        }
        bool ok = response.status == 200 && !response.timedOut && !response.networkError;
        if (section == "us-status") {
            // Session text only; a failed read removes it instead of showing an old session.
            if (ok && PublicEquityBoard::parseUsMarketStatus(response.body, &marketStatus)) market["us"] = marketStatus; else market.remove("us");
            received[section] = QDateTime::currentMSecsSinceEpoch();
            emit q->changed(); return;
        }
        if (ok) {
            if (section == "binance") ok = PublicEquityBoard::parseBinanceTickers(response.body,&parsed);
            else if (section == "coin") ok = PublicEquityBoard::parseCoins(response.body, &parsed);
            else if (section == "kr") ok = PublicEquityBoard::parseKorea(response.body, &parsed, &marketStatus);
            else ok = PublicEquityBoard::parseUs(response.body, &parsed);
        }
        if (ok) {
            const auto now = QDateTime::currentMSecsSinceEpoch();
            for (auto& item : parsed) { item.receivedAtMs = now; known[section + ":" + item.symbol] = item; }
            items[section] = parsed;
            received[section] = QDateTime::currentMSecsSinceEpoch();
            status.remove(section);
            if (!marketStatus.isEmpty()) market[section] = marketStatus; else market.remove(section);
        } else {
            // Keep the last good board; say why it may be old. A session label is
            // only true now, so a failed read removes it instead of keeping "장중".
            market.remove(section);
            status[section] = response.status == 429 ? QObject::tr("요청 제한 · 잠시 뒤 다시 조회")
                : response.timedOut || response.networkError ? QObject::tr("연결 실패 · 이전 시세 표시")
                : QObject::tr("응답 형식이 바뀌었거나 조회 실패 · 이전 시세 표시");
        }
        emit q->changed();
    }
};

PublicEquityBoard::PublicEquityBoard(TradingTransport* transport, QObject* parent, TradingTransport* rankingTransport)
    : QObject(parent), d_(std::make_unique<Private>()) {
    d_->q = this;
    d_->transport = transport;
    d_->separateRanking = rankingTransport && rankingTransport != transport;
    if (d_->separateRanking) d_->rankingTransport = rankingTransport;
    if (transport) {
        connect(transport, &TradingTransport::finished, this, [this](quint64 token, const HttpResult& r) { d_->finished(token, r); });
        connect(transport, &QObject::destroyed, this, [this] {
            if (d_->token) d_->status[d_->current] = tr("연결 실패 · 이전 시세 표시");
            d_->token = 0; d_->current.clear(); d_->queue.clear();
            emit changed();
        });
    }
    if (d_->separateRanking) {
        connect(rankingTransport, &TradingTransport::finished, this, [this](quint64 token, const HttpResult& r) { d_->rankingFinished(token, r); });
        connect(rankingTransport, &QObject::destroyed, this, [this] {
            if (d_->rankingToken) d_->status["binance"] = tr("연결 실패 · 이전 시세 표시");
            d_->rankingToken = 0;
            emit changed();
        });
    }
}

PublicEquityBoard::~PublicEquityBoard() {
    const bool boardActive = d_->token != 0, rankingActive = d_->rankingToken != 0;
    d_->token = d_->rankingToken = 0;
    d_->queue.clear();
    if (d_->transport) {
        disconnect(d_->transport, nullptr, this, nullptr);
        if (boardActive) d_->transport->abort();
    }
    if (d_->rankingTransport) {
        disconnect(d_->rankingTransport, nullptr, this, nullptr);
        if (rankingActive) d_->rankingTransport->abort();
    }
}

void PublicEquityBoard::refreshBinanceRanking() {
    if(QDateTime::currentMSecsSinceEpoch()<d_->nextBinanceMs || d_->rankingToken ||
       (d_->token&&d_->current=="binance") || d_->queue.contains("binance"))return;
    if (d_->separateRanking) {
        if (!d_->rankingTransport) return;
        d_->rankingToken = d_->nextToken++;
        d_->nextBinanceMs = QDateTime::currentMSecsSinceEpoch() + UsRefreshMs;
        d_->rankingTransport->send(d_->rankingToken, request("binance"));
        return;
    }
    if (!d_->transport) return;
    d_->queue.prepend("binance");
    d_->next();
}

void PublicEquityBoard::refresh() {
    if (!d_->transport) return;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (const auto& section : {QStringLiteral("coin"), QStringLiteral("kr"), QStringLiteral("us"), QStringLiteral("us-status")}) {
        if (d_->queue.contains(section) || (d_->token && d_->current == section)) continue;
        if (section.startsWith("us") && d_->received.value(section) > 0 && now - d_->received.value(section) < UsRefreshMs) continue;
        d_->queue.append(section);
    }
    d_->next();
}

QList<PublicBoardItem> PublicEquityBoard::items(const QString& section) const { return d_->items.value(section); }
PublicBoardItem PublicEquityBoard::knownItem(const QString& section, const QString& symbol) const { return d_->known.value(section + ":" + symbol); }
qint64 PublicEquityBoard::receivedAtMs(const QString& section) const { return d_->received.value(section); }
QString PublicEquityBoard::status(const QString& section) const { return d_->status.value(section); }
QString PublicEquityBoard::marketStatus(const QString& section) const { return d_->market.value(section); }
bool PublicEquityBoard::marketBoardBusy() const { return d_->token != 0 || !d_->queue.isEmpty(); }
bool PublicEquityBoard::busy() const { return marketBoardBusy() || d_->rankingToken != 0; }

RequestSpec PublicEquityBoard::request(const QString& section) {
    RequestSpec spec;
    spec.operation = Operation::Markets;
    spec.method = "GET";
    spec.headers.insert("Accept", "application/json");
    spec.headers.insert("User-Agent", "Mozilla/5.0 (KEEFI read-only market map)");
    if (section == "coin") {
        QUrl url("https://api.coingecko.com/api/v3/coins/markets");
        QUrlQuery query;
        query.addQueryItem("vs_currency", "krw"); query.addQueryItem("order", "market_cap_desc");
        query.addQueryItem("per_page", "250"); query.addQueryItem("page", "1"); query.addQueryItem("sparkline", "true");
        url.setQuery(query); spec.url = url;
    } else if (section == "kr") {
        QUrl url("https://m.stock.naver.com/api/stocks/marketValue/KOSPI");
        QUrlQuery query; query.addQueryItem("page", "1"); query.addQueryItem("pageSize", QString::number(KoreaLimit));
        url.setQuery(query); spec.url = url;
    } else if (section == "binance") {
        spec.url=QUrl("https://api.binance.com/api/v3/ticker/24hr?symbolStatus=TRADING");
    } else if (section == "us-status") {
        spec.url = QUrl("https://api.nasdaq.com/api/market-info");
    } else {
        QUrl url("https://api.nasdaq.com/api/screener/stocks");
        QUrlQuery query; query.addQueryItem("tableonly", "true"); query.addQueryItem("download", "true");
        query.addQueryItem("exchange", "nasdaq");
        url.setQuery(query); spec.url = url;
    }
    return spec;
}

bool PublicEquityBoard::isBoardRequest(const RequestSpec& r) {
    if (r.method != "GET" || r.authenticated || !r.body.isEmpty() || r.operation != Operation::Markets) return false;
    for (auto it = r.headers.cbegin(); it != r.headers.cend(); ++it)
        if (it.key() != "Accept" && it.key() != "User-Agent") return false;
    for (const auto& section : {QStringLiteral("coin"), QStringLiteral("kr"), QStringLiteral("us"), QStringLiteral("us-status"), QStringLiteral("binance")})
        if (r.url == request(section).url) return true;
    return false;
}

bool PublicEquityBoard::parseBinanceTickers(const QByteArray& body,QList<PublicBoardItem>* out){
    const auto doc=QJsonDocument::fromJson(body);
    if(!out||!doc.isArray()||doc.array().size()>10000)return false;
    QList<PublicBoardItem> rows;QSet<QString> seen;
    for(const auto& value:doc.array()){
        const auto ticker=value.toObject();
        const auto symbol=ticker.value("symbol").toString();
        if(!symbol.endsWith("USDT"))continue; // Joined with verified spot metadata before display.
        PublicBoardItem row;row.id=row.symbol=symbol;
        row.price=ticker.value("lastPrice").toString();row.changePercent=ticker.value("priceChangePercent").toString();
        bool priceOk=false,rateOk=false,volumeOk=false;
        const auto price=row.price.toDouble(&priceOk),rate=row.changePercent.toDouble(&rateOk);
        row.tradedValue=ticker.value("quoteVolume").toString().toDouble(&volumeOk);
        row.sourceTimeMs=ticker.value("closeTime").toInteger();
        bool unitsOk=false; const auto units=ticker.value("volume").toString().toDouble(&unitsOk);
        if(unitsOk&&std::isfinite(units)&&units>=0)row.tradedUnits=units;
        if(!priceOk||!std::isfinite(price)||price<=0||!rateOk||!std::isfinite(rate)||!volumeOk||
           !std::isfinite(row.tradedValue)||row.tradedValue<=0||row.sourceTimeMs<=0||ticker.value("count").toInteger()<=0)continue;
        if(seen.contains(symbol))return false;
        seen.insert(symbol);rows.append(row);
    }
    if(rows.isEmpty())return false;
    *out=rows;return true;
}

QString PublicEquityBoard::usSessionKst(const QDateTime& now) {
    const QTimeZone newYork("America/New_York"), seoul("Asia/Seoul");
    if (!newYork.isValid() || !seoul.isValid()) return QObject::tr("09:30–16:00 ET");
    const auto day = now.toTimeZone(newYork).date();
    const auto open = QDateTime(day, QTime(9, 30), newYork).toTimeZone(seoul);
    const auto close = QDateTime(day, QTime(16, 0), newYork).toTimeZone(seoul);
    return open.toString("HH:mm") + QStringLiteral("–") + close.toString("HH:mm") + QStringLiteral(" KST");
}

bool PublicEquityBoard::parseCoins(const QByteArray& body, QList<PublicBoardItem>* out) {
    const auto document = QJsonDocument::fromJson(body);
    if (!document.isArray()) return false;
    static const QSet<QString> stable{"usdt", "usdc", "dai", "usds", "usde", "fdusd", "tusd", "pyusd", "usd1", "busd", "susde", "usdtb", "rlusd", "bsc-usd",
        "usdg", "usdf", "usdd", "usdy", "usyc", "buidl", "xaut", "paxg", "eursafo", "ousg", "ustb", "benji"};
    // Tokenized real-world assets (loans, funds, treasuries, gold) are not coins on this map.
    static const QStringList realWorld{"Heloc", "Fund", "Gold", "Treasur", "Dollar", "Yield", "Bond", "Tokenized", " USD"};
    static const QRegularExpression coinSymbol(QStringLiteral("\\A[A-Z0-9]{2,10}\\z"));
    QList<PublicBoardItem> rows;
    QHash<QString,qsizetype> displayedSymbols;
    for (const auto& value : document.array()) {
        const auto o = value.toObject();
        const auto symbol = o.value("symbol").toString().toUpper(), name = o.value("name").toString();
        const double cap = o.value("market_cap").toDouble(), price = o.value("current_price").toDouble();
        if (symbol.isEmpty() || name.isEmpty() || !(cap > 0) || !(price > 0) || !std::isfinite(cap)) continue;
        if (!coinSymbol.match(symbol).hasMatch() ||
            std::any_of(realWorld.cbegin(), realWorld.cend(), [&](const QString& word) { return name.contains(word); })) continue;
        if (stable.contains(symbol.toLower()) || name.contains("Wrapped") || name.contains("Staked") || name.contains("Bridged") ||
            name.contains("Lido") || name.contains("wstETH") || name.contains("Liquid")) continue;
        const auto displayed = displayedSymbols.constFind(symbol);
        if (displayed != displayedSymbols.cend()) {
            rows[displayed.value()].ambiguousSymbol = true;
            continue;
        }
        // Preserve the existing display limit while checking the rest of this
        // response for duplicates of symbols already displayed.
        if (rows.size() >= CoinLimit) continue;
        displayedSymbols.insert(symbol, rows.size());
        PublicBoardItem item;
        item.id = symbol; item.symbol = symbol; item.name = name; item.group = coinGroup(symbol); item.currency = "KRW";
        item.sourceId = o.value("id").toString();
        item.price = price >= 100 ? number(std::round(price), 0) : price >= 1 ? number(price, 2) : QString::number(price, 'g', 6);
        const auto change = o.value("price_change_percentage_24h");
        item.changePercent = change.isDouble() ? signedPercent(change.toDouble()) : QString{};
        item.marketCap = cap;
        const double volume = o.value("total_volume").toDouble();
        item.tradedValue = volume > 0 && std::isfinite(volume) ? volume : 0;
        const auto hourly = o.value("sparkline_in_7d").toObject().value("price").toArray();
        if (hourly.size() >= 25) {
            QList<double> last;
            for (auto i = hourly.size() - 25; i < hourly.size(); ++i) { const double v = hourly[i].toDouble(); if (!(v > 0) || !std::isfinite(v)) { last.clear(); break; } last.append(v); }
            item.sparkline = last;
        }
        rows.append(item);
    }
    if (rows.isEmpty()) return false;
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.marketCap > b.marketCap; });
    *out = rows;
    return true;
}

bool PublicEquityBoard::parseUsMarketStatus(const QByteArray& body, QString* status) {
    if (!status || body.size() > 64 * 1024) return false;
    const auto value = QJsonDocument::fromJson(body).object().value("data").toObject().value("mrktStatus").toString();
    if (value == "Open") *status = QObject::tr("장중");
    else if (value == "Pre-Market") *status = QObject::tr("프리마켓");
    else if (value == "After-Hours") *status = QObject::tr("애프터마켓");
    else if (value == "Closed") *status = QObject::tr("장 마감");
    else return false;
    return true;
}

bool PublicEquityBoard::parseKorea(const QByteArray& body, QList<PublicBoardItem>* out, QString* marketStatus) {
    const auto o = QJsonDocument::fromJson(body).object();
    const auto stocks = o.value("stocks").toArray();
    if (stocks.isEmpty()) return false;
    static const QRegularExpression code(QStringLiteral("\\A[0-9A-Z]{6}\\z"));
    QList<PublicBoardItem> rows;
    for (const auto& value : stocks) {
        const auto s = value.toObject();
        const auto id = s.value("itemCode").toString(), name = s.value("stockName").toString();
        const auto price = plain(s.value("closePrice").toString()), capText = plain(s.value("marketValue").toString());
        auto ratio = s.value("fluctuationsRatio").toString().trimmed();
        const auto direction = s.value("compareToPreviousPrice").toObject().value("code").toString();
        if (!code.match(id).hasMatch() || name.isEmpty() || !decimalText(price) || !decimalText(capText)) continue;
        // Naver codes: 1 상한, 2 상승, 3 보합, 4 하한, 5 하락. The ratio may be unsigned.
        if ((direction == "4" || direction == "5") && !ratio.startsWith('-')) ratio.prepend('-');
        PublicBoardItem item;
        item.id = id; item.symbol = id; item.name = name; item.currency = "KRW";
        item.group = koreaSectors().value(id, QStringLiteral("기타"));
        item.price = price;
        item.changePercent = decimalText(ratio) ? ratio : QString{};
        item.marketCap = capText.toDouble() * 1e8; // marketValue is in 억원
        const auto traded = plain(s.value("accumulatedTradingValueRaw").toString()); // KRW
        if (decimalText(traded)) item.tradedValue = traded.toDouble();
        const auto units = plain(s.value("accumulatedTradingVolume").toString());
        if (decimalText(units)) item.tradedUnits = units.toDouble();
        rows.append(item);
    }
    if (rows.isEmpty()) return false;
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.marketCap > b.marketCap; });
    if (marketStatus) {
        const auto status = o.value("marketStatus").toString();
        *marketStatus = status == "OPEN" ? QObject::tr("장중") : status == "CLOSE" ? QObject::tr("장 마감") : status.isEmpty() ? QString{} : status;
    }
    *out = rows;
    return true;
}

bool PublicEquityBoard::parseUs(const QByteArray& body, QList<PublicBoardItem>* out) {
    const auto rowsValue = QJsonDocument::fromJson(body).object().value("data").toObject().value("rows");
    if (!rowsValue.isArray()) return false;
    static const QRegularExpression symbolRe(QStringLiteral("\\A[A-Z]{1,5}(?:\\.[A-Z])?\\z"));
    QList<PublicBoardItem> rows;
    for (const auto& value : rowsValue.toArray()) {
        const auto r = value.toObject();
        const auto symbol = r.value("symbol").toString().trimmed();
        const double cap = plain(r.value("marketCap").toString()).toDouble();
        auto price = r.value("lastsale").toString().trimmed();
        auto change = r.value("pctchange").toString().trimmed();
        price.remove('$'); change.remove('%');
        if (!symbolRe.match(symbol).hasMatch() || !(cap > 0) || !decimalText(plain(price))) continue;
        PublicBoardItem item;
        item.id = symbol; item.symbol = symbol; item.currency = "USD";
        item.name = r.value("name").toString().section(" Common Stock", 0, 0).section(" Class ", 0, 0).trimmed();
        item.group = usSector(r.value("sector").toString());
        item.price = plain(price);
        item.changePercent = decimalText(change) ? change : QString{};
        item.marketCap = cap;
        const double shares = plain(r.value("volume").toString()).toDouble();
        if (shares > 0 && std::isfinite(shares)) item.tradedUnits = shares;
        if (shares > 0) item.tradedValue = shares * plain(price).toDouble(); // dollar volume
        rows.append(item);
    }
    if (rows.isEmpty()) return false;
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.marketCap > b.marketCap; });
    if (rows.size() > UsLimit) rows = rows.mid(0, UsLimit);
    *out = rows;
    return true;
}

} // namespace keefi
