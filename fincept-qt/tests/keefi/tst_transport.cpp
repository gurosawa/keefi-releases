#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include "keefi/BuildPolicy.h"
#include "keefi/TradingTransport.h"
#include "keefi/TradingRequestLimits.h"
#include "keefi/FxHistory.h"
#include <QLocale>
#include <QDateTime>
#include "keefi/ResponseJournal.h"
#include "keefi/PublicEquityBoard.h"
#include <QTest>
#include <QUrlQuery>
using namespace keefi;

// Answers each public board read at once; the Korean board's reply is chosen by the test.
class SessionBoardTransport final : public TradingTransport {
  public:
    int krStatus = 200; QByteArray krBody; int reads = 0;
    void abort() override {}
    void send(quint64 token, const RequestSpec& request) override {
        ++reads; HttpResult response; response.status = 200;
        if (request.url.host() == "m.stock.naver.com") { response.status = krStatus; response.body = krBody; }
        else if (request.url.path() == "/api/market-info") response.body = R"({"data":{"mrktStatus":"Closed"}})";
        else response.status = 503;
        emit finished(token, response);
    }
};
class TransportTest final : public QObject {
    Q_OBJECT
  private slots:
    void ecbHistoryResponseAllowanceIsLimitedToItsExactPublicRead() {
        const auto valid = FxHistory::ecbHistoryRequest();
        QVERIFY(NetworkTradingTransport::allowed(valid));
        QCOMPARE(NetworkTradingTransport::responseBodyLimit(valid), FxHistory::MaximumEcbBytes);
        QList<RequestSpec> rejected;
        auto bad = valid; bad.authenticated = true; rejected.append(bad);
        bad = valid; bad.method = "POST"; rejected.append(bad);
        bad = valid; bad.body = "{}"; rejected.append(bad);
        bad = valid; bad.operation = Operation::Account; rejected.append(bad);
        bad = valid; bad.url.setScheme("http"); rejected.append(bad);
        bad = valid; bad.url.setHost("example.invalid"); rejected.append(bad);
        bad = valid; bad.url.setPort(8443); rejected.append(bad);
        bad = valid; bad.url.setUserName("synthetic"); rejected.append(bad);
        bad = valid; bad.url.setFragment("other"); rejected.append(bad);
        bad = valid; bad.url.setQuery("other=1"); rejected.append(bad);
        bad = valid; bad.url.setPath("/stats/eurofxref/eurofxref-hist.zip"); rejected.append(bad);
        for (const auto* name : {"Authorization", "Cookie", "X-MBX-APIKEY", "Host"}) {
            bad = valid; bad.headers.insert(name, "SYNTHETIC"); rejected.append(bad);
        }
        for (const auto& request : rejected) {
            QVERIFY2(!NetworkTradingTransport::allowed(request), qPrintable(request.url.toString()));
            QCOMPARE(NetworkTradingTransport::responseBodyLimit(request), MaxStandardResponseBytes);
        }
    }
    void standardHttpRetryAfterPreservesServerDeadline() {
        HttpResult response;response.status=429;
        const auto until=QDateTime::currentDateTimeUtc().addSecs(120);
        response.headers["Retry-After"]=QLocale::c().toString(until,"ddd, dd MMM yyyy HH:mm:ss 'GMT'").toLatin1();
        const auto wait=TradingRequestLimits::retryAfterMs(response);
        QVERIFY2(wait>=118000 && wait<=120000,qPrintable(QString::number(wait)));
        response.status=418;QVERIFY(TradingRequestLimits::retryAfterMs(response)>=118000);
        response.localRetryAfterSeconds=150;QCOMPARE(TradingRequestLimits::retryAfterMs(response),qint64(150000));
        response.localRetryAfterSeconds=0;response.headers["Retry-After"]="invalid";
        QCOMPARE(TradingRequestLimits::retryAfterMs(response),qint64(60000));
        response.status=200;QCOMPARE(TradingRequestLimits::retryAfterMs(response),qint64(0));
    }
    void coinFlowReadsUseOnlyUnsignedObservedMarketRoutes() {
        const QStringList urls{
            "https://api.upbit.com/v1/trades/ticks?market=KRW-SYN&count=500",
            "https://api.upbit.com/v1/trades/ticks?market=KRW-SYN&count=500&cursor=9007199254740993&days_ago=1",
            "https://api.binance.com/api/v3/aggTrades?symbol=SYNUSDT&limit=500&fromId=9007199254740993",
            "https://api.binance.com/api/v3/aggTrades?symbol=SYNUSDT&limit=500&startTime=1728000000000&endTime=1728000059999",
            "https://fapi.binance.com/fapi/v1/aggTrades?symbol=SYNUSDT&limit=500",
            "https://fapi.binance.com/futures/data/openInterestHist?symbol=SYNUSDT&period=5m&limit=30"};
        for(const auto& url:urls){
            RequestSpec r;r.operation=Operation::Reference;r.url=QUrl(url);
            QVERIFY2(NetworkTradingTransport::allowed(r),qPrintable(url));
            auto bad=r;bad.authenticated=true;QVERIFY(!NetworkTradingTransport::allowed(bad));
            for(const auto* key:{"Authorization","Cookie","X-MBX-APIKEY","X-API-Key"}){
                bad=r;bad.headers[key]="SYNTHETIC";QVERIFY(!NetworkTradingTransport::allowed(bad));
            }
            bad=r;bad.method="POST";QVERIFY(!NetworkTradingTransport::allowed(bad));
            bad=r;bad.body="{}";QVERIFY(!NetworkTradingTransport::allowed(bad));
            bad=r;auto query=QUrlQuery(bad.url);query.addQueryItem("symbol","OTHER");bad.url.setQuery(query);
            QVERIFY(!NetworkTradingTransport::allowed(bad));
            bad=r;bad.operation=Operation::History;QVERIFY(!NetworkTradingTransport::allowed(bad));
            bad=r;bad.url.setPort(8443);QVERIFY(!NetworkTradingTransport::allowed(bad));
        }
    }
    void coinFlowReadBoundsNeverPermitPrivateOrAmbiguousRecovery() {
        const QStringList rejected{
            "https://api.upbit.com/v1/trades/ticks?market=KRW-SYN&count=500&days_ago=0",
            "https://api.upbit.com/v1/trades/ticks?market=KRW-SYN&count=500&days_ago=8",
            "https://api.upbit.com/v1/trades/ticks?market=KRW-SYN&count=501",
            "https://api.upbit.com/v1/trades/ticks?market=KRW-SYN&count=500&to=23:59:59",
            "https://api.binance.com/api/v3/aggTrades?symbol=SYNUSDT&limit=500&fromId=9&startTime=1728000000000&endTime=1728000059999",
            "https://api.binance.com/api/v3/aggTrades?symbol=SYNUSDT&limit=500&fromId=-1",
            "https://fapi.binance.com/fapi/v1/aggTrades?symbol=SYNUSDT&limit=500&startTime=1728000000000&endTime=1728003600000",
            "https://fapi.binance.com/fapi/v1/aggTrades?symbol=SYNUSDT&limit=1000",
            "https://fapi.binance.com/futures/data/openInterestHist?symbol=SYNUSDT&period=1m&limit=30",
            "https://fapi.binance.com/futures/data/openInterestHist?symbol=SYNUSDT&period=5m&limit=31",
            "https://fapi.binance.com/fapi/v1/allForceOrders?symbol=SYNUSDT&limit=500"};
        for(const auto& url:rejected){RequestSpec r;r.operation=Operation::Reference;r.url=QUrl(url);
            QVERIFY2(!NetworkTradingTransport::allowed(r),qPrintable(url));}
    }
    void yahooStockHistoryIsUnavailable_data() {
        QTest::addColumn<QString>("host");
        QTest::newRow("global") << QString("finance.yahoo.com");
        QTest::newRow("uk") << QString("uk.finance.yahoo.com");
    }
    void yahooStockHistoryIsUnavailable() {
        QFETCH(QString,host);
        RequestSpec request;request.operation=Operation::Markets;
        request.url=QUrl(QString("https://%1/quote/SYN/history/").arg(host));request.headers["Accept"]="text/html";
        QVERIFY(!NetworkTradingTransport::allowed(request));
        QCOMPARE(NetworkTradingTransport::responseBodyLimit(request),MaxStandardResponseBytes);
        request.authenticated=true;QVERIFY(!NetworkTradingTransport::allowed(request));
        request.headers["Authorization"]="Bearer SYNTHETIC_CHART_BEARER";
        QVERIFY(!NetworkTradingTransport::allowed(request));
    }
    void legacyPublicStockChartRoutesAreUnavailable_data() {
        QTest::addColumn<QString>("url");
        QTest::newRow("domestic") << QString("https://api.stock.naver.com/chart/domestic/item/005930?periodType=day");
        QTest::newRow("us") << QString("https://api.nasdaq.com/api/quote/AAPL/chart?assetclass=stocks");
    }
    void legacyPublicStockChartRoutesAreUnavailable() {
        QFETCH(QString,url);RequestSpec request;request.operation=Operation::Markets;request.url=QUrl(url);
        request.headers={{"Accept","application/json"},{"User-Agent","Mozilla/5.0 (KEEFI public watchlist)"}};
        QVERIFY(!NetworkTradingTransport::allowed(request));
    }
    void publicStockQuoteBoardsRemainAvailable() {
        for(const auto* section:{"kr","us"})QVERIFY(NetworkTradingTransport::allowed(PublicEquityBoard::request(section)));
        const auto session=PublicEquityBoard::request("us-status");QVERIFY(NetworkTradingTransport::allowed(session));QVERIFY(PublicEquityBoard::isBoardRequest(session));
        QCOMPARE(session.url,QUrl("https://api.nasdaq.com/api/market-info"));QVERIFY(!session.authenticated);
        QString text;const QList<QPair<QByteArray,QString>> sessions{{"Open","장중"},{"Pre-Market","프리마켓"},{"After-Hours","애프터마켓"},{"Closed","장 마감"}};
        for(const auto& [raw,shown]:sessions){QVERIFY(PublicEquityBoard::parseUsMarketStatus("{\"data\":{\"mrktStatus\":\""+raw+"\"}}",&text));QCOMPARE(text,shown);}
        QVERIFY(!PublicEquityBoard::parseUsMarketStatus("{\"data\":{\"mrktStatus\":\"Halted\"}}",&text));QVERIFY(!PublicEquityBoard::parseUsMarketStatus("not json",&text));
        QVERIFY(QUrlQuery(PublicEquityBoard::request("coin").url).queryItemValue("sparkline")=="true");
        const auto coin=[](const QJsonArray& prices){return QJsonDocument(QJsonArray{QJsonObject{{"id","synthetic-coin"},{"symbol","syn"},{"name","Synthetic"},{"market_cap",1e12},{"current_price",100.0},
            {"price_change_percentage_24h",1.0},{"total_volume",1e9},{"sparkline_in_7d",QJsonObject{{"price",prices}}}}}).toJson(QJsonDocument::Compact);};
        QJsonArray hourly;for(int i=0;i<30;++i)hourly.append(100.0+i);QList<PublicBoardItem> rows;
        QVERIFY(PublicEquityBoard::parseCoins(coin(hourly),&rows));QCOMPARE(rows.first().sparkline.size(),25);QCOMPARE(rows.first().sparkline.first(),105.0);QCOMPARE(rows.first().sparkline.last(),129.0);
        hourly[28]=-1.0;QVERIFY(PublicEquityBoard::parseCoins(coin(hourly),&rows));QVERIFY(rows.first().sparkline.isEmpty());
        QVERIFY(PublicEquityBoard::parseCoins(coin(QJsonArray{1.0,2.0}),&rows));QVERIFY(rows.first().sparkline.isEmpty());
    }
    void domesticSessionLabelNeverOutlivesAFailedRead() {
        // "장중" is only true now: a failed read or a reply without a session removes it.
        const auto korea=[](const QByteArray& status){return QByteArray(R"({)")+(status.isEmpty()?QByteArray():"\"marketStatus\":\""+status+"\",")+
            R"("stocks":[{"itemCode":"005930","stockName":"가상전자","closePrice":"70,000","fluctuationsRatio":"1.00","compareToPreviousPrice":{"code":"2"},"marketValue":"4,000,000"}]})";};
        SessionBoardTransport transport;PublicEquityBoard board(&transport);
        transport.krBody=korea("OPEN");board.refresh();QCOMPARE(board.marketStatus("kr"),QStringLiteral("장중"));
        transport.krStatus=500;board.refresh();QVERIFY(board.marketStatus("kr").isEmpty());QCOMPARE(board.items("kr").size(),1); // Old prices stay, labelled old.
        transport.krStatus=200;board.refresh();QCOMPARE(board.marketStatus("kr"),QStringLiteral("장중"));
        transport.krBody=korea({});board.refresh();QVERIFY(board.marketStatus("kr").isEmpty());
    }
    void stockChartReadsUseExactMarketOnlyRoutes() {
        const QStringList urls{"https://openapi.tossinvest.com/api/v1/candles?symbol=SYN&interval=1m&count=200&adjusted=true",
            "https://openapi.tossinvest.com/api/v1/stocks?symbols=SYN", "https://openapi.tossinvest.com/api/v1/market-calendar/US?date=2026-01-02",
            "https://openapi.tossinvest.com/api/v1/price-limits?symbol=000001", "https://openapi.tossinvest.com/api/v1/stocks/000001/warnings",
            "https://openapi.tossinvest.com/api/v1/stocks/000001/investor-trading?count=10", "https://openapi.tossinvest.com/api/v1/stocks/000001/program-trades?count=10"};
        for(const auto& url:urls){RequestSpec r;r.operation=Operation::Reference;r.authenticated=true;r.url=QUrl(url);r.headers["Accept"]="application/json";r.headers["Authorization"]="Bearer SYNTHETIC_CHART_BEARER";
            QVERIFY2(NetworkTradingTransport::allowed(r),qPrintable(r.url.path()));
            auto bad=r;bad.method="POST";QVERIFY(!NetworkTradingTransport::allowed(bad));bad=r;bad.body="synthetic";QVERIFY(!NetworkTradingTransport::allowed(bad));
            bad=r;auto q=QUrlQuery(bad.url);q.addQueryItem("account","synthetic");bad.url.setQuery(q);QVERIFY(!NetworkTradingTransport::allowed(bad));
            bad=r;bad.url.setPath("/api/v1/accounts");QVERIFY(!NetworkTradingTransport::allowed(bad));
        }
    }
    void stockChartAuxiliaryRoutesAcceptVerifiedAlphanumericDomesticSymbols() {
        for(const auto* suffix:{"warnings","investor-trading","program-trades"}){
            RequestSpec request;request.operation=Operation::Reference;request.authenticated=true;
            request.url=QUrl(QString("https://openapi.tossinvest.com/api/v1/stocks/0101N0/")+suffix);
            request.headers["Accept"]="application/json";
            QVERIFY(NetworkTradingTransport::allowed(request));
            for(const auto* symbol:{"0101n0","0101N00","0101N","01%2F0N0"}){
                auto invalid=request;invalid.url=QUrl(QString("https://openapi.tossinvest.com/api/v1/stocks/")+symbol+'/'+suffix);
                QVERIFY(!NetworkTradingTransport::allowed(invalid));
            }
        }
    }
    void hyperliquidReadsUseOnlyExactUnsignedInfoBodies_data() {
        QTest::addColumn<int>("operation");
        QTest::addColumn<QString>("type");
        QTest::addColumn<bool>("needsUser");
        QTest::newRow("mode") << int(Operation::AccountConfig) << QString("userAbstraction") << true;
        QTest::newRow("metadata") << int(Operation::Rules) << QString("metaAndAssetCtxs") << false;
        QTest::newRow("perps") << int(Operation::Account) << QString("clearinghouseState") << true;
        QTest::newRow("unified-spot") << int(Operation::FundingBalance) << QString("spotClearinghouseState") << true;
    }
    void hyperliquidReadsUseOnlyExactUnsignedInfoBodies() {
        QFETCH(int, operation); QFETCH(QString, type); QFETCH(bool, needsUser);
        const QString address = "0x1111111111111111111111111111111111111111";
        QJsonObject body{{"type", type}};
        if (needsUser) body["user"] = address;
        RequestSpec r; r.operation = Operation(operation); r.method = "POST";
        r.url = QUrl("https://api.hyperliquid.xyz/info");
        r.headers["Content-Type"] = "application/json";
        r.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
        QVERIFY(NetworkTradingTransport::allowed(r));
        auto bad = r; bad.authenticated = true; QVERIFY(!NetworkTradingTransport::allowed(bad));
        for (const auto* method : {"GET", "PUT", "DELETE"}) {
            bad = r; bad.method = method; QVERIFY(!NetworkTradingTransport::allowed(bad));
        }
        for (const auto* host : {"api.hyperliquid.xyz.invalid", "api.hyperliquid-testnet.xyz", "hyperliquid.xyz"}) {
            bad = r; bad.url.setHost(host); QVERIFY(!NetworkTradingTransport::allowed(bad));
        }
        for (const auto* path : {"/exchange", "/info/", "/api/info"}) {
            bad = r; bad.url.setPath(path); QVERIFY(!NetworkTradingTransport::allowed(bad));
        }
        bad = r; bad.url.setScheme("http"); QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.url.setPort(8443); QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.url.setQuery("type=clearinghouseState"); QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.url.setFragment("private"); QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.url.setUserInfo("fake:fake"); QVERIFY(!NetworkTradingTransport::allowed(bad));
        for (const auto* key : {"Authorization", "Host", "Cookie", "X-API-Key", "Proxy-Authorization", "Accept"}) {
            bad = r; bad.headers[key] = "FAKE"; QVERIFY(!NetworkTradingTransport::allowed(bad));
        }
        bad = r; bad.headers.clear(); QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.headers["content-type"] = "application/json"; QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.headers["Content-Type"] = "text/plain"; QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.headers["Content-Type"] = "application/json\r\nAuthorization: fake"; QVERIFY(!NetworkTradingTransport::allowed(bad));
        for (auto forbidden : {Operation::PlaceOrder, Operation::TestOrder, Operation::CancelOrder,
                               Operation::AmendOrder, Operation::Positions, Operation::Authenticate}) {
            bad = r; bad.operation = forbidden; QVERIFY(!NetworkTradingTransport::allowed(bad));
        }
        for (const auto* key : {"signature", "dex", "vaultAddress", "unknown"}) {
            auto extra = body; extra[key] = "fake";
            bad = r; bad.body = QJsonDocument(extra).toJson(QJsonDocument::Compact); QVERIFY(!NetworkTradingTransport::allowed(bad));
        }
        auto changed = body; changed.remove("type");
        bad = r; bad.body = QJsonDocument(changed).toJson(QJsonDocument::Compact); QVERIFY(!NetworkTradingTransport::allowed(bad));
        changed = body; changed["type"] = "exchange";
        bad = r; bad.body = QJsonDocument(changed).toJson(QJsonDocument::Compact); QVERIFY(!NetworkTradingTransport::allowed(bad));
        // This producer sends Qt Compact JSON only; ambiguous or noncanonical bodies are refused.
        bad = r; bad.body.prepend(' '); QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.body.chop(1); bad.body += ",\"type\":\"" + type.toUtf8() + "\"}";
        QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.body.replace("\"type\"", "\"\\u0074ype\""); QVERIFY(!NetworkTradingTransport::allowed(bad));
        if (needsUser) {
            changed = body; changed.remove("user");
            bad = r; bad.body = QJsonDocument(changed).toJson(QJsonDocument::Compact); QVERIFY(!NetworkTradingTransport::allowed(bad));
            for (const auto* invalid : {"", "11111111111111111111111111111111", "0x1234",
                                        "0x0000000000000000000000000000000000000000",
                                        "0xgggggggggggggggggggggggggggggggggggggggg"}) {
                changed = body; changed["user"] = invalid;
                bad = r; bad.body = QJsonDocument(changed).toJson(QJsonDocument::Compact); QVERIFY(!NetworkTradingTransport::allowed(bad));
            }
            bad = r; bad.body.chop(1); bad.body += ",\"user\":\"" + address.toUtf8() + "\"}";
            QVERIFY(!NetworkTradingTransport::allowed(bad));
            bad = r; bad.body = "{\"user\":\"" + address.toUtf8() + "\",\"type\":\"" + type.toUtf8() + "\"}";
            QVERIFY(!NetworkTradingTransport::allowed(bad));
        } else {
            changed = body; changed["user"] = address;
            bad = r; bad.body = QJsonDocument(changed).toJson(QJsonDocument::Compact); QVERIFY(!NetworkTradingTransport::allowed(bad));
        }
    }
    void okxDerivativePositionsKeepSpotRulesClosed_data() {
        QTest::addColumn<QString>("instrumentType");
        QTest::newRow("swap") << QString("SWAP");
        QTest::newRow("futures") << QString("FUTURES");
    }
    void okxDerivativePositionsKeepSpotRulesClosed() {
        QFETCH(QString, instrumentType);
        for (const auto* host : {"openapi.okx.com", "us.okx.com", "eea.okx.com"}) {
            RequestSpec r; r.operation = Operation::Positions; r.authenticated = true;
            r.url = QUrl(QString("https://%1/api/v5/account/positions?instType=%2").arg(host, instrumentType));
            for (const auto* key : {"OK-ACCESS-KEY", "OK-ACCESS-SIGN", "OK-ACCESS-TIMESTAMP", "OK-ACCESS-PASSPHRASE"}) r.headers[key] = "FAKE";
            QVERIFY(NetworkTradingTransport::allowed(r));
            auto bad = r; bad.authenticated = false; QVERIFY(!NetworkTradingTransport::allowed(bad));
            bad = r; bad.method = "POST"; QVERIFY(!NetworkTradingTransport::allowed(bad));
            bad = r; bad.body = "{}"; QVERIFY(!NetworkTradingTransport::allowed(bad));
            bad = r; bad.headers["x-simulated-trading"] = "1"; QVERIFY(!NetworkTradingTransport::allowed(bad));
            bad = r; bad.headers["Host"] = "other.invalid"; QVERIFY(!NetworkTradingTransport::allowed(bad));
            bad = r; bad.url.setQuery(QString{}); QVERIFY(!NetworkTradingTransport::allowed(bad));
            for (const auto* query : {"instType=SPOT", "instType=OPTION", "instType=SWAP&instType=FUTURES", "instType=SWAP&instId=BTC-USDT-SWAP"}) {
                bad = r; bad.url.setQuery(query); QVERIFY(!NetworkTradingTransport::allowed(bad));
            }
            bad = r; bad.url.setPath("/api/v5/account/set-leverage"); QVERIFY(!NetworkTradingTransport::allowed(bad));
            bad = r; bad.operation = Operation::Rules; bad.url.setPath("/api/v5/account/instruments");
            QVERIFY(!NetworkTradingTransport::allowed(bad));
            bad = r; bad.operation = Operation::Markets; bad.authenticated = false; bad.headers.clear();
            bad.url.setPath("/api/v5/public/instruments"); QVERIFY(!NetworkTradingTransport::allowed(bad));
        }
    }
    void retiredPaperAndTestnetHostsNeverLeaveTheTransport() {
        // 2026-10-21: KEEFI keeps live connections only. Each live request below stays allowed;
        // the same request to the retired demo, testnet or paper server is refused.
        struct Pair { Operation operation; QByteArray method; const char* live; const char* retired; bool authenticated; };
        const Pair pairs[]{
            {Operation::Time, "GET", "https://api.binance.com/api/v3/time", "https://testnet.binance.vision/api/v3/time", false},
            {Operation::Time, "GET", "https://fapi.binance.com/fapi/v1/time", "https://demo-fapi.binance.com/fapi/v1/time", false},
            {Operation::Authenticate, "POST", "https://api.kiwoom.com/oauth2/token", "https://mockapi.kiwoom.com/oauth2/token", true}};
        for (const auto& pair : pairs) {
            RequestSpec live; live.operation = pair.operation; live.method = pair.method; live.authenticated = pair.authenticated;
            if (pair.method == "POST") live.body = "{}";
            live.url = QUrl(pair.live);
            QVERIFY2(NetworkTradingTransport::allowed(live), pair.live);
            auto retired = live; retired.url = QUrl(pair.retired);
            QVERIFY2(!NetworkTradingTransport::allowed(retired), pair.retired);
        }
        RequestSpec okx; okx.operation=Operation::Account; okx.authenticated=true;
        okx.url=QUrl("https://openapi.okx.com/api/v5/account/balance");
        for(const auto& key:{"OK-ACCESS-KEY","OK-ACCESS-SIGN","OK-ACCESS-TIMESTAMP","OK-ACCESS-PASSPHRASE"})okx.headers[key]="FAKE";
        QVERIFY(NetworkTradingTransport::allowed(okx));
        okx.headers["x-simulated-trading"]="1";QVERIFY(!NetworkTradingTransport::allowed(okx));
    }
    void responseJournalExcludesAuthenticationResponses() {
        // R1: broker sign-in replies carry reusable session tokens. They must never reach the journal.
        ResponseJournal::setEnabled(true);
        const QList<QPair<QString, QByteArray>> replies{
            {"https://openapi.koreainvestment.com:9443/oauth2/tokenP", R"({"access_token":"FAKE_ACCESS_TOKEN_R1","token_type":"Bearer","expires_in":3600})"},
            {"https://openapi.ls-sec.co.kr:8080/oauth2/token", R"({"access_token":"FAKE_ACCESS_TOKEN_R1","expires_in":86400})"},
            {"https://openapi.tossinvest.com/oauth2/token", R"({"access_token":"FAKE_ACCESS_TOKEN_R1","expires_in":"3600"})"},
            {"https://api.kiwoom.com/oauth2/token", R"({"token":"FAKE_KIWOOM_TOKEN_R1","token_type":"bearer"})"},
            {"https://openapi.koreainvestment.com:9443/oauth2/tokenP", R"({"error_description":"FAKE_ACCESS_TOKEN_R1 rejected"})"}};
        for (const auto& [url, body] : replies) {
            HttpResult reply; reply.status = body.contains("error") ? 403 : 200; reply.body = body;
            ResponseJournal::recordTrading(Operation::Authenticate, "POST", QUrl(url), false, reply);
        }
        HttpResult balance; balance.status = 200; balance.body = R"({"output":[{"pdno":"005930","hldg_qty":"1"}]})";
        ResponseJournal::recordTrading(Operation::Holdings, "GET", QUrl("https://openapi.koreainvestment.com:9443/uapi/x"), true, balance);
        const auto entries = ResponseJournal::entries();
        for (const auto& entry : entries) {
            QVERIFY(!entry.body.contains("FAKE_ACCESS_TOKEN_R1") && !entry.body.contains("FAKE_KIWOOM_TOKEN_R1"));
            QVERIFY(!entry.body.toBase64().contains(QByteArray("FAKE_ACCESS_TOKEN_R1").toBase64()));
        }
        QCOMPARE(entries.size(), 1); // Ordinary account reads are still kept.
        QCOMPARE(entries.first().path, QStringLiteral("/uapi/x"));
        ResponseJournal::setEnabled(false);
        QVERIFY(ResponseJournal::entries().isEmpty());
    }
    void responseJournalKeepsOnlyHostPathAndBodiesWhileEnabled() {
        ResponseJournal::setEnabled(false);
        HttpResult reply; reply.status = 200; reply.body = R"([{"currency":"BTC","balance":"0.5"}])";
        const QUrl signedUrl("https://api.example.invalid/v1/accounts?account=12345678&signature=SECRET");
        ResponseJournal::record("trading", "Account", "GET", signedUrl, true, reply);
        QVERIFY(ResponseJournal::entries().isEmpty()); // Off by default: nothing is kept.
        ResponseJournal::setEnabled(true);
        ResponseJournal::record("trading", "Account", "GET", signedUrl, true, reply);
        HttpResult large; large.status = 200; large.body = QByteArray(ResponseJournal::BodyLimit + 1, 'x');
        ResponseJournal::record("trading", "Markets", "GET", QUrl("https://api.example.invalid/catalogue"), false, large);
        const auto entries = ResponseJournal::entries();
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries[0].host, QStringLiteral("api.example.invalid"));
        QCOMPARE(entries[0].path, QStringLiteral("/v1/accounts"));
        QCOMPARE(entries[0].body, reply.body);
        QVERIFY(entries[0].authenticated && entries[0].receivedAtMs > 0 && entries[0].bodySha256.size() == 64);
        for (const auto& field : {entries[0].host, entries[0].path, entries[0].operation, entries[0].method})
            QVERIFY(!field.contains("12345678") && !field.contains("SECRET"));
        QVERIFY(entries[1].bodyOmitted && entries[1].body.isEmpty() && entries[1].bodySize == ResponseJournal::BodyLimit + 1);
        for (int i = 0; i < ResponseJournal::EntryLimit + 5; ++i)
            ResponseJournal::record("trading", "Quote", "GET", QUrl("https://api.example.invalid/q"), false, reply);
        QCOMPARE(ResponseJournal::entries().size(), ResponseJournal::EntryLimit);
        ResponseJournal::setEnabled(false); // Switching off drops everything.
        QVERIFY(ResponseJournal::entries().isEmpty());
    }
    void spotPublicRequestsCannotCarryFinancialAuthority() {
        for(const auto& host:{"api.upbit.com","api.binance.com"}) {
            const bool upbit=QByteArray(host)=="api.upbit.com";
            RequestSpec r;r.operation=Operation::Quote;r.url=QUrl(QString("https://%1%2").arg(host,upbit?"/v1/ticker?markets=KRW-BTC,KRW-USDT":"/api/v3/ticker/price?symbol=BTCUSDT"));
            QVERIFY(NetworkTradingTransport::allowed(r));
            auto bad=r;bad.url.setPath(upbit?"/v1/withdraws/coin":"/api/v3/order");QVERIFY(!NetworkTradingTransport::allowed(bad));
            bad=r;bad.method="POST";QVERIFY(!NetworkTradingTransport::allowed(bad));
            bad=r;bad.authenticated=true;QVERIFY(!NetworkTradingTransport::allowed(bad));
            for(const auto& header:{"Authorization","X-MBX-APIKEY","Host","Cookie","Proxy-Authorization"}) {
                bad=r;bad.headers[header]="FAKE";QVERIFY(!NetworkTradingTransport::allowed(bad));
            }
            bad=r;bad.url.setQuery(upbit?"markets=KRW-BTC&markets=KRW-ETH":"symbol=BTCUSDT&symbol=ETHUSDT");QVERIFY(!NetworkTradingTransport::allowed(bad));
            bad=r;bad.url.setQuery(upbit?"markets=KRW-BTC&unknown=1":"symbol=BTCUSDT&signature=fake");QVERIFY(!NetworkTradingTransport::allowed(bad));
            bad=r;bad.body="unexpected";QVERIFY(!NetworkTradingTransport::allowed(bad));
        }
    }
    void spotMutationsAreBoundToOperationMethodAndFields() {
        // Valid routes remain blocked in the read-only build; OFF exercises the full authority checks below.
        RequestSpec r;r.operation=Operation::PlaceOrder;r.method="POST";r.authenticated=true;
        r.url=QUrl("https://api.upbit.com/v1/orders");r.headers["Authorization"]="Bearer FAKE";
        r.body=R"({"market":"KRW-BTC","side":"bid","ord_type":"limit","volume":"1","price":"100","identifier":"fake"})";
        QCOMPARE(NetworkTradingTransport::allowed(r), !readOnlyBuild());
        auto bad=r;bad.operation=Operation::Account;QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad=r;bad.method="DELETE";QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad=r;bad.headers["authorization"]="Bearer DUPLICATE";QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad=r;bad.body.replace("\"identifier\":\"fake\"","\"identifier\":\"fake\",\"withdraw\":\"true\"");QVERIFY(!NetworkTradingTransport::allowed(bad));
        r={};r.operation=Operation::AmendOrder;r.method="POST";r.authenticated=true;r.headers["X-MBX-APIKEY"]="FAKE";
        r.url=QUrl("https://api.binance.com/api/v3/order/cancelReplace");
        r.body="symbol=BTCUSDT&cancelOrderId=1&cancelReplaceMode=STOP_ON_FAILURE&orderRateLimitExceededMode=DO_NOTHING&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=100&newClientOrderId=fake&newOrderRespType=RESULT&timestamp=100&recvWindow=5000&signature=fake";
        QCOMPARE(NetworkTradingTransport::allowed(r), !readOnlyBuild());
        bad=r;bad.operation=Operation::PlaceOrder;QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad=r;bad.body.replace("STOP_ON_FAILURE","ALLOW_FAILURE");QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad=r;bad.body.replace("type=LIMIT","type=MARKET");QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad=r;bad.body+="&signature=duplicate";QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad=r;bad.url.setQuery("signature=elsewhere");QVERIFY(!NetworkTradingTransport::allowed(bad));
    }
    void futuresAuthorityIsIsolatedFromSpotAndSettings() {
        RequestSpec r;r.operation=Operation::PlaceOrder;r.method="POST";r.authenticated=true;
        r.url=QUrl("https://fapi.binance.com/fapi/v1/order");r.headers["X-MBX-APIKEY"]="FAKE";
        r.body="symbol=BTCUSDT&side=BUY&type=LIMIT&positionSide=BOTH&quantity=1&price=100&timeInForce=GTC&newClientOrderId=fake&recvWindow=5000&timestamp=100&signature=fake";
        QCOMPARE(NetworkTradingTransport::allowed(r), !readOnlyBuild());
        for(const auto& path:{"/api/v3/order","/fapi/v1/leverage","/fapi/v1/marginType","/fapi/v1/positionSide/dual","/fapi/v1/batchOrders","/fapi/v1/algoOrder"}){
            auto bad=r;bad.url.setPath(path);QVERIFY(!NetworkTradingTransport::allowed(bad));}
        auto bad=r;bad.url.setHost("api.binance.com");QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad=r;bad.body+="&positionSide=LONG";QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad=r;bad.headers["Host"]="evil.invalid";QVERIFY(!NetworkTradingTransport::allowed(bad));
        r={};r.operation=Operation::Rules;r.url=QUrl("https://fapi.binance.com/fapi/v1/exchangeInfo");
        QVERIFY(NetworkTradingTransport::allowed(r));QCOMPARE(NetworkTradingTransport::responseBodyLimit(r),MaxBinanceMarketCatalogResponseBytes);
        r.headers["X-MBX-APIKEY"]="FAKE";QVERIFY(!NetworkTradingTransport::allowed(r));
        r={};r.operation=Operation::ApiRestrictions;r.authenticated=true;r.headers["X-MBX-APIKEY"]="FAKE";
        r.url=QUrl("https://api.binance.com/sapi/v1/account/apiRestrictions?timestamp=100&recvWindow=5000&signature=fake");
        QVERIFY(NetworkTradingTransport::allowed(r));r.url.setHost("demo-fapi.binance.com");QVERIFY(!NetworkTradingTransport::allowed(r));
    }
    void bithumbAuthorityAndQuoteAmountAreBounded() {
        RequestSpec r; r.operation=Operation::PlaceOrder; r.method="POST"; r.authenticated=true;
        r.url=QUrl("https://api.bithumb.com/v2/orders"); r.headers["Authorization"]="Bearer FAKE";
        r.body=R"({"market":"KRW-BTC","side":"bid","order_type":"price","price":"5000","client_order_id":"fake-new-1"})";
        QCOMPARE(NetworkTradingTransport::allowed(r), !readOnlyBuild());
        auto bad=r; bad.url.setPath("/v1/withdraws/coin"); QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad=r; bad.headers["Host"]="evil.invalid"; QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad=r; bad.headers["authorization"]="Bearer DUPLICATE"; QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad=r; bad.operation=Operation::TestOrder; QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad=r; bad.operation=Operation::AmendOrder; QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad=r; bad.body.replace("bid","ask"); QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad=r; bad.body.replace("\"price\":\"5000\"", "\"price\":\"5000\",\"volume\":\"1\""); QVERIFY(!NetworkTradingTransport::allowed(bad));
        r.operation=Operation::CancelOrder; r.method="DELETE"; r.body.clear();
        r.url=QUrl("https://api.bithumb.com/v2/order?order_id=fake-123"); QCOMPARE(NetworkTradingTransport::allowed(r), !readOnlyBuild());
        r.url.setQuery("order_id=fake-123&client_order_id=other"); QVERIFY(!NetworkTradingTransport::allowed(r));
        r.operation=Operation::Markets; r.method="GET"; r.authenticated=false;
        r.url=QUrl("https://api.bithumb.com/v1/market/all?isDetails=true");
        QVERIFY(!NetworkTradingTransport::allowed(r)); r.headers.clear(); QVERIFY(NetworkTradingTransport::allowed(r));
        r.operation=Operation::OpenOrders; r.authenticated=true; r.headers["Authorization"]="Bearer FAKE";
        r.url=QUrl("https://api.bithumb.com/v2/orders/pending?state=wait&limit=100&next_key=ab%2B%2F%3D%26z");
        QVERIFY(NetworkTradingTransport::allowed(r));
        r.url.setQuery("state=wait&market=KRW-BTC"); QVERIFY(!NetworkTradingTransport::allowed(r));
        r.url.setQuery("state=wait&state=watch"); QVERIFY(!NetworkTradingTransport::allowed(r));
    }
    void okxOnlyFixedCashOperationsCrossTheTransport() {
        RequestSpec r;r.operation=Operation::PlaceOrder;r.method="POST";r.authenticated=true;
        r.url=QUrl("https://openapi.okx.com/api/v5/trade/order");
        for(const auto& key:{"OK-ACCESS-KEY","OK-ACCESS-SIGN","OK-ACCESS-TIMESTAMP","OK-ACCESS-PASSPHRASE"})r.headers[key]="FAKE";
        r.body=R"({"instId":"BTC-USDT","tdMode":"cash","side":"buy","ordType":"limit","sz":"1","px":"100","clOrdId":"123","tradeQuoteCcy":"USDT","pxAmendType":"0"})";
        QCOMPARE(NetworkTradingTransport::allowed(r), !readOnlyBuild());
        for(const auto& host:{"us.okx.com","eea.okx.com"}){auto a=r;a.url.setHost(host);QCOMPARE(NetworkTradingTransport::allowed(a), !readOnlyBuild());}
        for(const auto& path:{"/api/v5/asset/transfer","/api/v5/asset/withdrawal","/api/v5/account/set-leverage","/api/v5/trade/batch-orders"}){auto a=r;a.url.setPath(path);QVERIFY(!NetworkTradingTransport::allowed(a));}
        auto a=r;a.headers["Host"]="evil.invalid";QVERIFY(!NetworkTradingTransport::allowed(a));
        a=r;a.headers["ok-access-key"]="DUPLICATE";QVERIFY(!NetworkTradingTransport::allowed(a));
        a=r;a.url.setPort(8443);QVERIFY(!NetworkTradingTransport::allowed(a));
        a=r;a.body.replace("cash","cross");QVERIFY(!NetworkTradingTransport::allowed(a));
        a=r;a.operation=Operation::Markets;a.method="GET";a.authenticated=false;a.url=QUrl("https://openapi.okx.com/api/v5/public/instruments?instType=SPOT");a.body.clear();
        QVERIFY(!NetworkTradingTransport::allowed(a));a.headers.clear();QVERIFY(NetworkTradingTransport::allowed(a));
        a.url.setQuery("instType=SPOT&instType=SWAP");QVERIFY(!NetworkTradingTransport::allowed(a));
    }
    void suspendedLsRoutesAreRefused_data() {
        QTest::addColumn<int>("operation"); QTest::addColumn<QString>("path"); QTest::addColumn<QByteArray>("tr");
        struct Route { Operation operation; const char* path; const char* tr; };
        const Route routes[]{
            {Operation::Accounts,"/stock/accno","CSPAQ12200"}, {Operation::Account,"/stock/accno","CSPAQ12200"},
            {Operation::Holdings,"/stock/accno","t0424"}, {Operation::BuyingPower,"/stock/accno","CSPBQ00200"},
            {Operation::OpenOrders,"/stock/accno","t0425"}, {Operation::History,"/stock/accno","CSPAQ13700"},
            {Operation::QueryOrder,"/stock/accno","CSPAQ13700"}, {Operation::QueryOrder,"/stock/accno","t0425"},
            {Operation::Markets,"/stock/etc","t8436"},
            {Operation::Rules,"/stock/market-data","t1102"}, {Operation::Quote,"/stock/market-data","t1102"},
            {Operation::PriceLimits,"/stock/market-data","t1102"}, {Operation::Depth,"/stock/market-data","t8450"},
            {Operation::AssetFilters,"/stock/market-data","t1404"}, {Operation::AssetFilters,"/stock/market-data","t1405"},
            {Operation::Reference,"/stock/chart","t8412"}, {Operation::TradingSchedule,"/etc/time-search","t0167"},
            {Operation::PlaceOrder,"/stock/order","CSPAT00601"}, {Operation::AmendOrder,"/stock/order","CSPAT00701"},
            {Operation::CancelOrder,"/stock/order","CSPAT00801"}};
        for (const auto& route : routes) {
            const auto name = QByteArray(route.tr) + '-' + QByteArray::number(int(route.operation));
            QTest::newRow(name.constData()) << int(route.operation) << QString(route.path) << QByteArray(route.tr);
        }
    }
    void suspendedLsRoutesAreRefused() {
        QFETCH(int, operation); QFETCH(QString, path); QFETCH(QByteArray, tr);
        RequestSpec r; r.operation = Operation(operation); r.method = "POST"; r.authenticated = true;
        r.url = QUrl("https://openapi.ls-sec.co.kr:8080" + path); r.headers["tr_cd"] = tr; r.body = "{}";
        QVERIFY(!NetworkTradingTransport::allowed(r));
        QCOMPARE(NetworkTradingTransport::responseBodyLimit(r), MaxStandardResponseBytes);
        auto bad = r; bad.operation = isMutation(r.operation) ? Operation::Holdings : Operation::PlaceOrder;
        QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.operation = Operation::TestOrder; QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.method = "GET"; QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.authenticated = false; QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.url.setPort(443); QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.url.setPort(-1); QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.url.setScheme("http"); QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.url.setHost("openapi.ls-sec.co.kr.invalid"); QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.url.setUserName("unexpected"); QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.url.setQuery("tr_cd=" + QString::fromLatin1(tr)); QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.url.setFragment("x"); QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.url.setPath(path + "/"); QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.headers["tr_cd"] = "unknown"; QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.headers.remove("tr_cd"); QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.headers["TR_CD"] = tr; QVERIFY(!NetworkTradingTransport::allowed(bad));
        bad = r; bad.headers.clear(); bad.headers["TR_CD"] = tr; QVERIFY(!NetworkTradingTransport::allowed(bad));
    }
    void suspendedLsAuthenticationIsRefused() {
        RequestSpec r; r.operation = Operation::Authenticate; r.method = "POST"; r.authenticated = true;
        r.url = QUrl("https://openapi.ls-sec.co.kr:8080/oauth2/token"); r.body = "grant_type=client_credentials";
        QVERIFY(!NetworkTradingTransport::allowed(r));
        r.headers["tr_cd"] = "CSPAT00601"; QVERIFY(!NetworkTradingTransport::allowed(r));
        r.headers.clear(); r.operation = Operation::PlaceOrder; QVERIFY(!NetworkTradingTransport::allowed(r));
        r.operation = Operation::Authenticate; r.url.setQuery("appkey=fake"); QVERIFY(!NetworkTradingTransport::allowed(r));
    }
    void kiwoomTrAuthority_data() {
        QTest::addColumn<int>("operation"); QTest::addColumn<QString>("path");
        QTest::addColumn<QByteArray>("apiId"); QTest::addColumn<QString>("host");
        struct Route { Operation operation; const char* path; const char* id; };
        const Route routes[]{
            {Operation::Accounts,"acnt","ka00001"}, {Operation::Markets,"stkinfo","ka10099"},
            {Operation::Rules,"stkinfo","ka10100"}, {Operation::Quote,"stkinfo","ka10001"},
            {Operation::Depth,"mrkcond","ka10004"}, {Operation::Reference,"chart","ka10080"},
            {Operation::Holdings,"acnt","kt00018"}, {Operation::Account,"acnt","kt00001"},
            {Operation::BuyingPower,"acnt","kt00011"}, {Operation::Commissions,"acnt","kt00010"},
            {Operation::OpenOrders,"acnt","ka10075"}, {Operation::History,"acnt","kt00007"},
            {Operation::QueryOrder,"acnt","kt00007"}, {Operation::PlaceOrder,"ordr","kt10000"},
            {Operation::PlaceOrder,"ordr","kt10001"}, {Operation::AmendOrder,"ordr","kt10002"},
            {Operation::CancelOrder,"ordr","kt10003"}};
        for (const auto* host : {"api.kiwoom.com"})
            for (const auto& route : routes) {
                const auto name = QByteArray(host) + '-' + route.id + '-' + QByteArray::number(int(route.operation));
                QTest::newRow(name.constData()) << int(route.operation) << QString("/api/dostk/") + route.path
                    << QByteArray(route.id) << QString(host);
            }
    }
    void kiwoomTrAuthority() {
        QFETCH(int, operation); QFETCH(QString, path); QFETCH(QByteArray, apiId); QFETCH(QString, host);
        RequestSpec valid; valid.operation = Operation(operation); valid.method = "POST"; valid.authenticated = true;
        valid.url = QUrl("https://" + host + path); valid.headers["api-id"] = apiId; valid.body = "{}";
        QCOMPARE(NetworkTradingTransport::allowed(valid), !readOnlyBuild() || !isMutation(valid.operation));
        QCOMPARE(NetworkTradingTransport::responseBodyLimit(valid), MaxStandardResponseBytes);
        auto modified = valid; modified.operation = Operation::TestOrder; QVERIFY(!NetworkTradingTransport::allowed(modified));
        modified = valid; modified.operation = isMutation(valid.operation) ? Operation::Holdings : Operation::PlaceOrder;
        QVERIFY(!NetworkTradingTransport::allowed(modified));
        modified = valid; modified.method = "GET"; QVERIFY(!NetworkTradingTransport::allowed(modified));
        modified = valid; modified.authenticated = false; QVERIFY(!NetworkTradingTransport::allowed(modified));
        modified = valid; modified.url.setHost(host + ".invalid"); QVERIFY(!NetworkTradingTransport::allowed(modified));
        modified = valid; modified.url.setPort(8080); QVERIFY(!NetworkTradingTransport::allowed(modified));
        modified = valid; modified.url.setScheme("http"); QVERIFY(!NetworkTradingTransport::allowed(modified));
        modified = valid; modified.url.setUserName("unexpected"); QVERIFY(!NetworkTradingTransport::allowed(modified));
        modified = valid; modified.url.setQuery("api-id=" + QString::fromLatin1(apiId)); QVERIFY(!NetworkTradingTransport::allowed(modified));
        modified = valid; modified.url.setFragment("x"); QVERIFY(!NetworkTradingTransport::allowed(modified));
        modified = valid; modified.url.setPath(path + "/"); QVERIFY(!NetworkTradingTransport::allowed(modified));
        modified = valid; modified.headers["api-id"] = "unknown"; QVERIFY(!NetworkTradingTransport::allowed(modified));
        modified = valid; modified.headers.remove("api-id"); QVERIFY(!NetworkTradingTransport::allowed(modified));
        modified = valid; modified.headers["API-ID"] = apiId; QVERIFY(!NetworkTradingTransport::allowed(modified));
        modified = valid; modified.headers.clear(); modified.headers["API-ID"] = apiId; QCOMPARE(NetworkTradingTransport::allowed(modified), !readOnlyBuild() || !isMutation(modified.operation));
    }
    void kiwoomTokenEndpointIsCredentialBootstrapOnly() {
        for (const auto* host : {"api.kiwoom.com"}) {
            RequestSpec r; r.operation = Operation::Authenticate; r.method = "POST"; r.authenticated = true;
            r.url = QUrl(QString("https://") + host + "/oauth2/token"); r.body = "{}";
            QVERIFY(NetworkTradingTransport::allowed(r));
            r.headers["api-id"] = "au10001"; QVERIFY(NetworkTradingTransport::allowed(r));
            r.headers["api-id"] = "kt10000"; QVERIFY(!NetworkTradingTransport::allowed(r));
            r.headers.clear(); r.operation = Operation::PlaceOrder; QVERIFY(!NetworkTradingTransport::allowed(r));
            r.operation = Operation::Authenticate; r.authenticated = false; QVERIFY(!NetworkTradingTransport::allowed(r));
        }
    }
};
QTEST_GUILESS_MAIN(TransportTest)
#include "tst_transport.moc"
