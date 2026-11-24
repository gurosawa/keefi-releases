#include "keefi/ChartInstrumentPicker.h"
#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/ChartInstrumentPresentation.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/PublicEquityBoard.h"
#include "keefi/TradingTransport.h"
#include "keefi/KeefiTheme.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalSpy>
#include <QTest>
#include <QTreeWidget>
using namespace keefi;
namespace {
class Transport final:public TradingTransport {
 public:
    QList<quint64> tokens;QList<RequestSpec> requests;
    void send(quint64 token,const RequestSpec& request)override{tokens.append(token);requests.append(request);}
    void abort()override{}
    void reply(int index,const QByteArray& body){HttpResult response;response.status=200;response.body=body;emit finished(tokens.at(index),response);}
};
QByteArray upbitMarkets(){return R"([{"market":"KRW-BTC","korean_name":"비트코인","english_name":"Bitcoin","market_event":{"warning":false,"caution":{}}}])";}
QByteArray binanceMarkets(){return R"({"symbols":[{"symbol":"HYPEUSDT","baseAsset":"HYPE","quoteAsset":"USDT","status":"TRADING","isSpotTradingAllowed":true},{"symbol":"BTCUSDT","baseAsset":"BTC","quoteAsset":"USDT","status":"TRADING","isSpotTradingAllowed":true},{"symbol":"ZECUSDT","baseAsset":"ZEC","quoteAsset":"USDT","status":"BREAK","isSpotTradingAllowed":false}]})";}
}
class ChartInstrumentPickerTest final:public QObject {
    Q_OBJECT
  private slots:
    void representativesUseExistingMarketsAndNeverInventMissingOnes();
    void searchAndActionsUseTheSelectedExactMarket();
    void emptySourcesCannotCreateARepresentativeList();
    void publicStocksStayStocksAndMarketFiltersChangeResults();
    void smallWindowKeepsFeedbackAndActionsOutsideTheResults();
    void losingThePublicCatalogueRemovesItsCandidates();
    void batchedLabelsKeepTheSingleIdentityContract();
};
void ChartInstrumentPickerTest::representativesUseExistingMarketsAndNeverInventMissingOnes(){
    Transport upbitTransport,binanceTransport;PublicMarketCatalog upbit(&upbitTransport);ChartInstrumentCatalog binance(&binanceTransport);
    upbit.refresh();upbitTransport.reply(0,upbitMarkets());binance.ensure("binance");binanceTransport.reply(0,binanceMarkets());
    ChartInstrumentPicker picker(&upbit,nullptr,&binance);
    QCOMPARE(picker.representativeIdentities(),QStringList({"coin:upbit:KRW-BTC","coin:binance:HYPEUSDT"}));
    auto* rows=picker.findChild<QTreeWidget*>("chartInstrumentResults");QVERIFY(rows);QCOMPARE(rows->topLevelItemCount(),2);
}
void ChartInstrumentPickerTest::searchAndActionsUseTheSelectedExactMarket(){
    Transport transport;ChartInstrumentCatalog binance(&transport);binance.ensure("binance");transport.reply(0,binanceMarkets());
    ChartInstrumentPicker picker(nullptr,nullptr,&binance);picker.setQuery("hype");picker.show();
    auto* rows=picker.findChild<QTreeWidget*>("chartInstrumentResults");QVERIFY(rows);QCOMPARE(rows->topLevelItemCount(),1);
    QSignalSpy open(&picker,&ChartInstrumentPicker::openRequested),add(&picker,&ChartInstrumentPicker::addRequested);
    auto* openButton=picker.findChild<QPushButton*>("chartInstrumentOpen");auto* addButton=picker.findChild<QPushButton*>("chartInstrumentAdd");QVERIFY(openButton);QVERIFY(addButton);
    QVERIFY(openButton->isEnabled());openButton->click();QCOMPARE(open.count(),1);QCOMPARE(open.first().first().toString(),QString("coin:binance:HYPEUSDT"));
    addButton->click();QCOMPARE(add.count(),1);QCOMPARE(add.first().first().toString(),QString("coin:binance:HYPEUSDT"));
    picker.setQuery("does-not-exist");QCOMPARE(rows->topLevelItemCount(),0);QVERIFY(!openButton->isEnabled());QVERIFY(!addButton->isEnabled());
}
void ChartInstrumentPickerTest::emptySourcesCannotCreateARepresentativeList(){
    ChartInstrumentPicker picker(nullptr,nullptr,nullptr);QVERIFY(picker.representativeIdentities().isEmpty());
    auto* button=picker.findChild<QPushButton*>("chartInstrumentAddRepresentatives");QVERIFY(button);QVERIFY(!button->isEnabled());
}
void ChartInstrumentPickerTest::publicStocksStayStocksAndMarketFiltersChangeResults(){
    Transport transport;PublicEquityBoard board(&transport);board.refresh();
    transport.reply(0,"[]"); // No coin board is required for stock discovery.
    transport.reply(1,R"({"stocks":[{"itemCode":"005930","stockName":"삼성전자","closePrice":"100","marketValue":"10"},{"itemCode":"000660","stockName":"SK하이닉스","closePrice":"200","marketValue":"9"}]})");
    transport.reply(2,R"({"data":{"rows":[{"symbol":"AAPL","name":"Apple Common Stock","lastsale":"100","marketCap":"1000"},{"symbol":"MSFT","name":"Microsoft Common Stock","lastsale":"200","marketCap":"2000"},{"symbol":"NVDA","name":"NVIDIA Common Stock","lastsale":"300","marketCap":"3000"}]}})");
    ChartInstrumentPicker picker(nullptr,&board,nullptr);
    QCOMPARE(picker.representativeIdentities(),QStringList({"stock:KR:005930","stock:KR:000660","stock:US:AAPL","stock:US:MSFT","stock:US:NVDA"}));
    QSignalSpy bulk(&picker,&ChartInstrumentPicker::addRepresentativesRequested);
    picker.findChild<QPushButton*>("chartInstrumentAddRepresentatives")->click();QCOMPARE(bulk.count(),1);
    QCOMPARE(bulk.first().first().toStringList(),picker.representativeIdentities());
    picker.setQuery("skhy");auto* rows=picker.findChild<QTreeWidget*>("chartInstrumentResults");QCOMPARE(rows->topLevelItemCount(),1);
    QCOMPARE(rows->topLevelItem(0)->data(0,Qt::UserRole).toString(),QString("stock:KR:000660"));
    picker.setQuery({});auto* filter=picker.findChild<QRadioButton*>("chartInstrumentFilter_us");QVERIFY(filter);filter->setChecked(true);QCOMPARE(rows->topLevelItemCount(),3);
    for(int i=0;i<rows->topLevelItemCount();++i)QVERIFY(rows->topLevelItem(i)->data(0,Qt::UserRole).toString().startsWith("stock:US:"));
}
void ChartInstrumentPickerTest::smallWindowKeepsFeedbackAndActionsOutsideTheResults(){
    Transport transport;ChartInstrumentCatalog binance(&transport);binance.ensure("binance");transport.reply(0,binanceMarkets());
    ChartInstrumentPicker picker(nullptr,nullptr,&binance);
    picker.setStyleSheet(KeefiTheme::localStyleSheet(&picker));
    picker.setFeedback(QStringLiteral("주식 차트는 계좌·연결에서 증권사를 연결하면 볼 수 있어요. 관심 목록에는 먼저 추가할 수 있어요."));
    picker.resize(540,440);picker.show();QTest::qWait(25);
    auto* rows=picker.findChild<QTreeWidget*>("chartInstrumentResults");
    auto* status=picker.findChild<QLabel*>("chartInstrumentStatus");
    auto* feedback=picker.findChild<QLabel*>("chartInstrumentFeedback");
    auto* button=picker.findChild<QPushButton*>("chartInstrumentAddRepresentatives");
    QVERIFY(rows&&status&&feedback&&button);
    QVERIFY2(rows->geometry().bottom()<status->geometry().top(),"Result rows and their status must not overlap at the smallest permitted size");
    QVERIFY(feedback->geometry().bottom()<button->mapTo(&picker,QPoint{}).y());
    QVERIFY(button->height()>=44);QVERIFY(picker.rect().contains(button->mapTo(&picker,button->rect().bottomRight())));
}
void ChartInstrumentPickerTest::losingThePublicCatalogueRemovesItsCandidates(){
    Transport transport;auto* upbit=new PublicMarketCatalog(&transport);
    upbit->refresh();transport.reply(0,upbitMarkets());
    ChartInstrumentPicker picker(upbit,nullptr,nullptr);QCOMPARE(picker.representativeIdentities().size(),1);
    delete upbit;
    QVERIFY(picker.representativeIdentities().isEmpty());
    QCOMPARE(picker.findChild<QTreeWidget*>("chartInstrumentResults")->topLevelItemCount(),0);
    QVERIFY(!picker.findChild<QPushButton*>("chartInstrumentAddRepresentatives")->isEnabled());
}
void ChartInstrumentPickerTest::batchedLabelsKeepTheSingleIdentityContract(){
    Transport marketHttp,binanceHttp,boardHttp;
    PublicMarketCatalog markets(&marketHttp);ChartInstrumentCatalog binance(&binanceHttp);PublicEquityBoard board(&boardHttp);
    markets.refresh();marketHttp.reply(0,upbitMarkets());binance.ensure("binance");binanceHttp.reply(0,binanceMarkets());
    board.refresh();boardHttp.reply(0,"[]");boardHttp.reply(1,R"({"stocks":[{"itemCode":"005930","stockName":"삼성전자","closePrice":"100","marketValue":"10"}]})");
    boardHttp.reply(2,R"({"data":{"rows":[{"symbol":"AAPL","name":"Apple Common Stock","lastsale":"100","marketCap":"1000"}]}})");
    ChartInstrumentPresentation presentation;presentation.setSources(&markets,&board,&binance);
    const QList<WatchlistIdentity> identities{{"coin","upbit","KRW-BTC"},{"coin","upbit","KRW-UNKNOWN"},{"coin","binance","HYPEUSDT"},{"coin","binance","ZECUSDT"},{"coin","bithumb","KRW-BTC"},{"stock","KR","005930"},{"stock","US","AAPL"},{"stock","US","UNKNOWN"},{}};
    const auto reads=marketHttp.requests.size()+binanceHttp.requests.size()+boardHttp.requests.size();const auto batch=presentation.labels(identities);
    for(const auto& identity:identities){const auto single=presentation.label(identity),value=batch.value(identity.key());QCOMPARE(value.name,single.name);QCOMPARE(value.code,single.code);QCOMPARE(value.market,single.market);}
    QCOMPARE(marketHttp.requests.size()+binanceHttp.requests.size()+boardHttp.requests.size(),reads);
    presentation.setSources(nullptr,nullptr,nullptr);const auto missing=presentation.labels(identities);
    for(const auto& identity:identities){const auto single=presentation.label(identity),value=missing.value(identity.key());QCOMPARE(value.text(),single.text());}
}
QTEST_MAIN(ChartInstrumentPickerTest)
#include "tst_chart_instrument_picker.moc"
