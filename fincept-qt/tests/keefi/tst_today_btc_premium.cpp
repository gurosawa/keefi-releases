#include "keefi/TodayBtcPremium.h"
#include "keefi/TradingTransport.h"
#include "keefi/WorldMarkets.h"
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QSet>
#include <QTest>
#include <QTimeZone>
using namespace keefi;
namespace {
using Read=TodayBtcPremium::Read;
using State=TodayBtcPremiumSnapshot::State;
constexpr qint64 Now=1793624400000; // 2026-11-02 13:00 UTC = 22:00 KST (Monday).
class Wire final:public TradingTransport {
  public:
    quint64 token=0;QList<RequestSpec> sent;int aborts=0;
    void send(quint64 next,const RequestSpec& r)override{token=next;sent.append(r);}
    void abort()override{++aborts;}
    void answer(const QByteArray& bytes,int status=200){HttpResult r;r.status=status;r.body=bytes;emit finished(token,r);}
};
// Hana Bank's USD/KRW posting as Naver Securities publishes it, posted at `at`.
QByteArray posting(qint64 at,const QString& price="1,400.00"){
    const auto local=QDateTime::fromMSecsSinceEpoch(at,QTimeZone("Asia/Seoul")).toString("yyyy-MM-dd'T'HH:mm:ss")+"+09:00";
    return QJsonDocument(QJsonObject{{"isSuccess",true},{"result",QJsonObject{{"categoryType","exchange"},{"reutersCode","FX_USDKRW"},
        {"closePrice",price},{"fluctuations","1.40"},{"fluctuationsRatio","0.10"},{"marketStatus","OPEN"},{"localTradedAt",local},
        {"stockExchangeType",QJsonObject{{"nameKor",QStringLiteral("하나은행")},{"delayTime",0}}}}}}).toJson();
}
QByteArray upbit(qint64 at,const QString& price="141400000"){
    return QJsonDocument(QJsonArray{QJsonObject{{"market","KRW-BTC"},{"trade_price",price},{"timestamp",QString::number(at)}}}).toJson();
}
QByteArray binance(qint64 at,const QString& price="100000"){return QJsonDocument(QJsonObject{{"symbol","BTCUSDT"},{"lastPrice",price},{"closeTime",QString::number(at)}}).toJson();}
void fill(Wire& u,Wire& b,Wire& f,qint64 now=Now){u.answer(upbit(now));b.answer(binance(now));f.answer(posting(now-60000));}
}
class TodayBtcPremiumTest:public QObject {
    Q_OBJECT
  private slots:
    void exactFormulaHasNoTetherBridge(){
        QCOMPARE(TodayBtcPremium::percent("141400000","100000","1400"),QString("1"));
        QCOMPARE(TodayBtcPremium::percent("142800000","100000","1400"),QString("2"));
        QCOMPARE(TodayBtcPremium::percent("138600000","100000","1400"),QString("-1"));
        QVERIFY(TodayBtcPremium::percent("141400000","100000","0").isEmpty());
    }
    void threeIndependentInputsPublishImmediately(){
        qint64 now=Now;Wire u,b,f;TodayBtcPremium model(&u,&b,&f,nullptr,[&]{return now;});QSignalSpy changed(&model,&TodayBtcPremium::changed);
        model.setActive(true);QCOMPARE(u.sent.size(),1);QCOMPARE(b.sent.size(),1);QCOMPARE(f.sent.size(),1);
        QCOMPARE(model.snapshot().reason,QStringLiteral("원·달러 환율 수신 전"));
        u.answer(upbit(now));b.answer(binance(now));QCOMPARE(model.snapshot().state,State::Waiting);
        f.answer(posting(now-60000));QCOMPARE(model.snapshot().state,State::Current);QCOMPARE(model.snapshot().percent,QString("1"));QCOMPARE(model.snapshot().checkedAtMs,now);
        QCOMPARE(model.snapshot().fx.postedDate,QDate(2026,11,2));QCOMPARE(model.snapshot().fx.postedAtMs,now-60000);QCOMPARE(model.snapshot().fx.krwPerUsd,QString("1400"));
        QCOMPARE(f.sent.first().url,WorldMarkets::request(WorldMarketItem::UsdKrw).url); // The Today board's exact Hana posting read.
        QVERIFY(changed.count()>=4);
        // No selected coin, Coinbase, notice, calendar or breadth input exists.
        model.refresh();QCOMPARE(u.sent.size(),1);QCOMPARE(b.sent.size(),1);QCOMPARE(f.sent.size(),1);
    }
    void missingInputsAreSpecific(){
        qint64 now=Now;Wire u,b,f;TodayBtcPremium model(&u,&b,&f,nullptr,[&]{return now;});model.setActive(true);f.answer(posting(now-60000));
        QCOMPARE(model.snapshot().fx.postedDate,QDate(2026,11,2));
        QCOMPARE(model.snapshot().reason,QStringLiteral("업비트 BTC 원화가 수신 전"));u.answer(upbit(now));
        QCOMPARE(model.snapshot().reason,QStringLiteral("바이낸스 BTC 현물가 수신 전"));
        b.answer("{}",503);QVERIFY(model.snapshot().reason.contains(QStringLiteral("바이낸스")));QCOMPARE(model.snapshot().state,State::Waiting);
    }
    void lastGoodSurvivesSlowFxFailureAndRecovery(){
        qint64 now=Now;Wire u,b,f;TodayBtcPremium model(&u,&b,&f,nullptr,[&]{return now;});model.setActive(true);fill(u,b,f,now);
        const auto good=model.snapshot();now+=TodayBtcPremium::FxRefreshMs;model.refresh();QCOMPARE(f.sent.size(),2);
        u.answer(upbit(now));b.answer(binance(now));
        now+=TodayBtcPremium::LateMs;model.refresh();u.answer(upbit(now));b.answer(binance(now));
        QCOMPARE(model.snapshot().state,State::Recent);QCOMPARE(model.snapshot().percent,QString("1"));QVERIFY(model.snapshot().reason.contains(QStringLiteral("환율")));
        const auto checked=model.snapshot().checkedAtMs;
        f.answer("",503);QCOMPARE(model.snapshot().percent,QString("1"));QCOMPARE(model.snapshot().checkedAtMs,checked);
        now+=60000;model.refresh();u.answer(upbit(now,"142800000"));b.answer(binance(now));f.answer(posting(now-1000));
        QCOMPARE(model.snapshot().state,State::Current);QCOMPARE(model.snapshot().percent,QString("2"));QVERIFY(model.snapshot().checkedAtMs>good.checkedAtMs);
    }
    void failedLaneDoesNotBlockOtherLanesAndHonorsRetryAfter(){
        qint64 now=Now;Wire u,b,f;TodayBtcPremium model(&u,&b,&f,nullptr,[&]{return now;});model.setActive(true);fill(u,b,f,now);
        now+=15000;model.refresh();HttpResult limit;limit.status=429;limit.headers.insert("Retry-After","120");emit u.finished(u.token,limit);b.answer(binance(now));
        const auto count=u.sent.size();now+=30000;model.refresh();QCOMPARE(u.sent.size(),count);QCOMPARE(b.sent.size(),3);QCOMPARE(f.sent.size(),1);
        QCOMPARE(model.snapshot().state,State::Recent);QVERIFY(model.snapshot().reason.contains(QStringLiteral("요청 제한")));
        now+=90000;model.refresh();QCOMPARE(u.sent.size(),count+1);
    }
    void retryAfterHttpDateAndLocalCooldownAreRespected(){
        qint64 now=Now;Wire u,b,f;TodayBtcPremium model(&u,&b,&f,nullptr,[&]{return now;});
        QCOMPARE(u.sent.size(),0);QCOMPARE(b.sent.size(),0);QCOMPARE(f.sent.size(),0);model.setActive(true);
        HttpResult limited;limited.status=429;limited.headers.insert("rEtRy-AfTeR","Mon, 02 Nov 2026 13:02:00 GMT");emit f.finished(f.token,limited);
        now+=119000;model.refresh();QCOMPARE(f.sent.size(),1);now+=1000;model.refresh();QCOMPARE(f.sent.size(),2);
        HttpResult local;local.localRetryAfterSeconds=180;emit f.finished(f.token,local);
        now+=179000;model.refresh();QCOMPARE(f.sent.size(),2);now+=1000;model.refresh();QCOMPARE(f.sent.size(),3);
    }
    void olderQuoteCannotOverwriteLatestObservedPrice(){
        qint64 now=Now;Wire u,b,f;TodayBtcPremium model(&u,&b,&f,nullptr,[&]{return now;});model.setActive(true);fill(u,b,f,now);
        now+=15000;model.refresh();u.answer(upbit(Now-1,"1"));b.answer(binance(now));
        QCOMPARE(model.snapshot().state,State::Recent);QCOMPARE(model.snapshot().percent,QString("1"));
        QVERIFY(model.snapshot().reason.contains(QStringLiteral("이전 응답")));
    }
    void weekendPostingStaysUsableWhileItIsReRead(){
        qint64 now=Now;Wire u,b,f;TodayBtcPremium model(&u,&b,&f,nullptr,[&]{return now;});model.setActive(true);
        // Friday's last posting, read on Monday night: the posting is old, the read is fresh.
        const qint64 friday=QDateTime(QDate(2026,10,30),QTime(18,0),QTimeZone("Asia/Seoul")).toMSecsSinceEpoch();
        u.answer(upbit(now));b.answer(binance(now));f.answer(posting(friday));QCOMPARE(model.snapshot().state,State::Current);
        QCOMPARE(model.snapshot().fx.postedDate,QDate(2026,10,30));
        now+=120000;model.refresh();u.answer(upbit(now));b.answer(binance(now));QCOMPARE(model.snapshot().state,State::Current);
        QCOMPARE(f.sent.size(),2); // Re-read every minute; the answer has not come back yet.
        now+=TodayBtcPremium::QuoteFreshnessMs+1;QCOMPARE(model.snapshot().state,State::Recent);
    }
    void failedRateReadKeepsLastValueAsRecent(){
        qint64 now=Now;Wire u,b,f;TodayBtcPremium model(&u,&b,&f,nullptr,[&]{return now;});model.setActive(true);fill(u,b,f,now);
        now+=TodayBtcPremium::FxRefreshMs;model.refresh();u.answer(upbit(now));b.answer(binance(now));QCOMPARE(f.sent.size(),2);
        f.answer("",503);const auto failed=model.snapshot();
        QCOMPARE(failed.state,State::Recent);QCOMPARE(failed.percent,QString("1"));QVERIFY(failed.reason.contains(QStringLiteral("원·달러")));
    }
    void olderPostingCannotReplaceNewerOne(){
        qint64 now=Now;Wire u,b,f;TodayBtcPremium model(&u,&b,&f,nullptr,[&]{return now;});model.setActive(true);fill(u,b,f,now);
        now+=TodayBtcPremium::FxRefreshMs;model.refresh();u.answer(upbit(now));b.answer(binance(now));
        f.answer(posting(Now-120000,"1,300.00")); // Posted before the posting already in use.
        const auto kept=model.snapshot();
        QCOMPARE(kept.state,State::Recent);QCOMPARE(kept.percent,QString("1"));QCOMPARE(kept.fx.krwPerUsd,QString("1400"));QCOMPARE(kept.fx.postedAtMs,Now-60000);
        QCOMPARE(kept.reason,QStringLiteral("원·달러 고시 시각이 이전 응답보다 과거예요"));
        now+=TodayBtcPremium::FxRefreshMs;model.refresh();u.answer(upbit(now));b.answer(binance(now));f.answer(posting(now-1000));
        QCOMPARE(model.snapshot().state,State::Current); // The next newer posting clears it.
    }
    void unansweredRateReadIsLate(){
        qint64 now=Now;Wire u,b,f;TodayBtcPremium model(&u,&b,&f,nullptr,[&]{return now;});model.setActive(true);fill(u,b,f,now);
        now+=TodayBtcPremium::FxRefreshMs;model.refresh();u.answer(upbit(now));b.answer(binance(now));QCOMPARE(model.snapshot().state,State::Current);
        now+=TodayBtcPremium::LateMs;model.refresh();u.answer(upbit(now));b.answer(binance(now)); // The posting read never comes back.
        QCOMPARE(model.snapshot().state,State::Recent);QCOMPARE(model.snapshot().reason,QStringLiteral("원·달러 환율 응답 지연"));
    }
    void skewedCryptoLegsCannotPublishNewCurrentValue(){
        qint64 now=Now;Wire u,b,f;TodayBtcPremium model(&u,&b,&f,nullptr,[&]{return now;});model.setActive(true);
        u.answer(upbit(now-31000));b.answer(binance(now));f.answer(posting(now-60000));QCOMPARE(model.snapshot().state,State::Waiting);
        QVERIFY(model.snapshot().reason.contains("30"));
    }
    void stopRejectsLateReplyAndResumePreservesLastGood(){
        qint64 now=Now;Wire u,b,f;TodayBtcPremium model(&u,&b,&f,nullptr,[&]{return now;});model.setActive(true);
        const auto old=u.token;model.setActive(false);QCOMPARE(u.aborts,1);HttpResult r;r.status=200;r.body=upbit(now);emit u.finished(old,r);QCOMPARE(model.snapshot().state,State::Waiting);
        model.setActive(true);QVERIFY(u.token!=old);fill(u,b,f,now);QCOMPARE(model.snapshot().percent,QString("1"));model.setActive(false);QCOMPARE(model.snapshot().percent,QString("1"));
    }
    // An exchange clock a little ahead of this computer's (Codex saw Binance 39 ms ahead on
    // 2026-11-06). The reply waits, unparsed and unshown, until the local clock reaches its
    // time, and keeps both the exchange time and its first receipt time.
    void slightlyEarlyExchangeClockIsHeldThenAccepted_data(){
        QTest::addColumn<int>("lane");QTest::addColumn<qint64>("lead");
        QTest::newRow("binance 1ms")<<1<<qint64(1);QTest::newRow("binance 39ms")<<1<<qint64(39);QTest::newRow("binance 2s")<<1<<qint64(2000);
        QTest::newRow("upbit 39ms")<<0<<qint64(39);QTest::newRow("upbit 2s")<<0<<qint64(2000);
    }
    void slightlyEarlyExchangeClockIsHeldThenAccepted(){
        QFETCH(int,lane);QFETCH(qint64,lead);
        qint64 now=Now,mono=0;Wire u,b,f;TodayBtcPremium model(&u,&b,&f,nullptr,[&]{return now;},[&]{return mono;});model.setActive(true);
        f.answer(posting(now-60000));
        if(lane==0){b.answer(binance(now));u.answer(upbit(now+lead));}else{u.answer(upbit(now));b.answer(binance(now+lead));}
        auto& wire=lane==0?u:b;
        QCOMPARE(model.snapshot().state,State::Waiting);
        now+=lead-1;mono+=lead-1;model.refresh();
        QCOMPARE(model.snapshot().state,State::Waiting);QCOMPARE(wire.sent.size(),1); // Still held; no second request meanwhile.
        now+=1;mono+=1;model.refresh();
        const auto s=model.snapshot();QCOMPARE(s.state,State::Current);QCOMPARE(s.percent,QString("1"));QCOMPARE(s.checkedAtMs,Now+lead);
        const auto& e=lane==0?s.upbit.evidence:s.binance.evidence;QCOMPARE(e.sourceTimeMs,Now+lead);QCOMPARE(e.receivedAtMs,Now);
        // The strict parser itself is unchanged: on its own it still refuses any future time.
        AssetQuote quote;QVERIFY(!TodayBtcPremium::parseQuote(lane==0?Read::UpbitBtc:Read::BinanceBtc,lane==0?upbit(Now+1):binance(Now+1),Now,&quote));
    }
    void heldReplyKeepsThePreviousValueUntilReleased(){
        qint64 now=Now,mono=0;Wire u,b,f;TodayBtcPremium model(&u,&b,&f,nullptr,[&]{return now;},[&]{return mono;});model.setActive(true);fill(u,b,f,now);
        now+=TodayBtcPremium::QuoteRefreshMs;mono+=TodayBtcPremium::QuoteRefreshMs;model.refresh();u.answer(upbit(now));b.answer(binance(now+39,"90000"));
        auto s=model.snapshot();QCOMPARE(s.state,State::Current);QCOMPARE(s.percent,QString("1"));QCOMPARE(s.binance.price,QString("100000")); // Not shown yet.
        now+=39;mono+=39;model.refresh();s=model.snapshot();
        QCOMPARE(s.state,State::Current);QCOMPARE(s.binance.price,QString("90000"));QVERIFY(s.percent!=QString("1"));
    }
    void exchangeClockMoreThanTwoSecondsAheadIsRejectedWithTheGap(){
        qint64 now=Now,mono=0;Wire u,b,f;TodayBtcPremium model(&u,&b,&f,nullptr,[&]{return now;},[&]{return mono;});model.setActive(true);fill(u,b,f,now);
        now+=TodayBtcPremium::QuoteRefreshMs;mono+=TodayBtcPremium::QuoteRefreshMs;model.refresh();u.answer(upbit(now));b.answer(binance(now+2001));
        const auto s=model.snapshot();QCOMPARE(s.state,State::Recent);QCOMPARE(s.percent,QString("1"));
        QCOMPARE(s.reason,QStringLiteral("바이낸스 BTC 현물가 시각이 이 컴퓨터 시계보다 2.1초 앞서요 · 시간 설정 확인"));
        // The usual minute before the next read; nothing is re-read in between.
        now+=59999;mono+=59999;model.refresh();QCOMPARE(b.sent.size(),2);now+=1;mono+=1;model.refresh();QCOMPARE(b.sent.size(),3);
        // A wrong market that is also ahead is reported as a failed check, not as a clock gap.
        auto wrong=binance(now+2001);wrong.replace("BTCUSDT","BNBUSDT");b.answer(wrong);
        QCOMPARE(model.snapshot().reason,QStringLiteral("바이낸스 BTC 현물가 수신 실패 또는 검증 실패"));
    }
    void heldReplyEndsAfterTwoSecondsEvenIfTheClockStepsBack(){
        qint64 now=Now,mono=0;Wire u,b,f;TodayBtcPremium model(&u,&b,&f,nullptr,[&]{return now;},[&]{return mono;});model.setActive(true);
        f.answer(posting(now-60000));u.answer(upbit(now));b.answer(binance(now+500));
        now-=10000;mono+=1999;model.refresh();QCOMPARE(model.snapshot().state,State::Waiting);QCOMPARE(b.sent.size(),1); // Still held.
        mono+=1;model.refresh(); // Two seconds of monotonic time end the wait; the time is still ahead, so it is refused.
        const auto s=model.snapshot();QCOMPARE(s.state,State::Waiting);QVERIFY(s.binance.price.isEmpty());
        QCOMPARE(s.reason,QStringLiteral("바이낸스 BTC 현물가 시각이 이 컴퓨터 시계보다 11초 앞서요 · 시간 설정 확인"));
    }
    void hidingDropsAHeldReplyAndResumeReadsAgain(){
        qint64 now=Now,mono=0;Wire u,b,f;TodayBtcPremium model(&u,&b,&f,nullptr,[&]{return now;},[&]{return mono;});model.setActive(true);
        f.answer(posting(now-60000));u.answer(upbit(now));b.answer(binance(now+39,"90000"));
        model.setActive(false);now+=39;mono+=39;model.refresh();QCOMPARE(model.snapshot().state,State::Waiting); // Dropped, never parsed.
        model.setActive(true);QCOMPARE(b.sent.size(),2);b.answer(binance(now));
        const auto s=model.snapshot();QCOMPARE(s.state,State::Current);QCOMPARE(s.binance.price,QString("100000"));QCOMPARE(s.percent,QString("1"));
    }
    void heldReplyIsReleasedByItsOwnTimer(){
        qint64 now=Now;Wire u,b,f;TodayBtcPremium model(&u,&b,&f,nullptr,[&]{return now;}); // Real monotonic clock and timers.
        model.setActive(true);f.answer(posting(now-60000));u.answer(upbit(now));b.answer(binance(now+39));
        QCOMPARE(model.snapshot().state,State::Waiting);now+=39;
        QTRY_COMPARE_WITH_TIMEOUT(model.snapshot().state,State::Current,500); // Before the one-second refresh tick.
    }
    void strictFxParserRejectsWrongMarketFutureStaleAndImplausible(){
        UsdKrwPosting fx;QString reason;
        QVERIFY2(TodayBtcPremium::parseFxPosting(posting(Now-60000,"1,344.90"),Now,&fx,&reason),qPrintable(reason));
        QCOMPARE(fx.krwPerUsd,QString("1344.9"));QCOMPARE(fx.postedAtMs,Now-60000);QCOMPARE(fx.postedDate,QDate(2026,11,2));QCOMPARE(fx.receivedAtMs,Now);
        auto other=posting(Now);other.replace("FX_USDKRW","FX_JPYKRW");QVERIFY(!TodayBtcPremium::parseFxPosting(other,Now,&fx,&reason));
        QVERIFY(!TodayBtcPremium::parseFxPosting(posting(Now+300000),Now,&fx,&reason));            // Posted in the future.
        QVERIFY(!TodayBtcPremium::parseFxPosting(posting(Now-8LL*86400000),Now,&fx,&reason));       // Older than seven days.
        QVERIFY(reason.contains(QStringLiteral("고시일")));
        QVERIFY(!TodayBtcPremium::parseFxPosting(posting(Now,"100.00"),Now,&fx,&reason));            // Implausible won rate.
        QVERIFY(!TodayBtcPremium::parseFxPosting(posting(Now,"1.344,90"),Now,&fx,&reason));          // Not a grouped decimal.
        QVERIFY(!TodayBtcPremium::parseFxPosting(R"({"isSuccess":false,"result":{}})",Now,&fx,&reason));
        QVERIFY(!TodayBtcPremium::parseFxPosting("<html>maintenance</html>",Now,&fx,&reason));
        QCOMPARE(fx.krwPerUsd,QString("1344.9")); // A rejected answer never replaces the last parsed posting.
    }
    void strictQuoteIdentityRejectsWrongAssetFutureAndStale(){
        AssetQuote quote;QVERIFY(TodayBtcPremium::parseQuote(Read::UpbitBtc,upbit(Now),Now,&quote));QCOMPARE(quote.baseAsset,QString("BTC"));
        auto wrong=upbit(Now);wrong.replace("KRW-BTC","KRW-USDT");QVERIFY(!TodayBtcPremium::parseQuote(Read::UpbitBtc,wrong,Now,&quote));
        QVERIFY(!TodayBtcPremium::parseQuote(Read::UpbitBtc,upbit(Now+1),Now,&quote));
        QVERIFY(!TodayBtcPremium::parseQuote(Read::UpbitBtc,upbit(Now-180001),Now,&quote));
        auto other=binance(Now);other.replace("BTCUSDT","BNBUSDT");QVERIFY(!TodayBtcPremium::parseQuote(Read::BinanceBtc,other,Now,&quote));
    }
    void actualPublicThreeLaneSnapshot(){
        if(qEnvironmentVariable("KEEFI_DEV86_LIVE_PUBLIC")!="1")QSKIP("opt-in live unsigned Upbit/Binance/Hana posting GET; separate from fixtures");
        NetworkTradingTransport upbitWire,binanceWire,fxWire;
        TodayBtcPremium model(&upbitWire,&binanceWire,&fxWire);QJsonArray replies;QSet<QString> answered;
        const auto observe=[&](NetworkTradingTransport* wire,const QString& name){connect(wire,&TradingTransport::finished,this,[&,name](quint64 token,const HttpResult& response){
            answered.insert(name);replies.append(QJsonObject{{"source",name},{"token",QString::number(token)},{"http_status",response.status},{"network_error",response.networkError},{"timed_out",response.timedOut},{"local_retry_after_seconds",response.localRetryAfterSeconds},{"received_at_ms",QString::number(QDateTime::currentMSecsSinceEpoch())}});
        });};
        observe(&upbitWire,"upbit-btc");observe(&binanceWire,"binance-btc-spot");observe(&fxWire,"hana-usdkrw-posting");
        QElapsedTimer elapsed;elapsed.start();model.setActive(true);
        while(answered.size()<3&&elapsed.elapsed()<20000)QTest::qWait(50);
        // A reply stamped up to two seconds ahead of this computer's clock waits that long.
        QTest::qWaitFor([&]{return model.snapshot().state==State::Current;},int(TodayBtcPremium::ClockLeadHoldMs)+1000);
        const auto result=model.snapshot();model.setActive(false); // No unattended refresh after the one capture.
        const auto lead=[](const AssetQuote& q){return QString::number(q.evidence.receivedAtMs>0?q.evidence.sourceTimeMs-q.evidence.receivedAtMs:0);};
        QJsonObject receipt{{"kind","real-public-data"},{"test","actualPublicThreeLaneSnapshot"},{"duration_ms",QString::number(elapsed.elapsed())},{"responses",replies},
            {"state",result.state==State::Current?"current":result.state==State::Recent?"recent":"waiting"},{"percent",result.percent},{"reason",result.reason},{"checked_at_ms",QString::number(result.checkedAtMs)},
            {"upbit_btc_krw",result.upbit.price},{"upbit_source_time_ms",QString::number(result.upbit.evidence.sourceTimeMs)},{"binance_btc_usdt",result.binance.price},{"binance_source_time_ms",QString::number(result.binance.evidence.sourceTimeMs)},
            {"upbit_source_minus_received_ms",lead(result.upbit)},{"binance_source_minus_received_ms",lead(result.binance)},{"clock_lead_hold_ms",QString::number(TodayBtcPremium::ClockLeadHoldMs)},
            {"usd_krw",result.fx.krwPerUsd},{"fx_posted_at_ms",QString::number(result.fx.postedAtMs)},{"fx_posted_date",result.fx.postedDate.toString(Qt::ISODate)},{"fx_received_at_ms",QString::number(result.fx.receivedAtMs)},{"assumption","1 USDT ≈ 1 USD"}};
        qInfo().noquote()<<QJsonDocument(receipt).toJson(QJsonDocument::Compact);
        const auto path=qEnvironmentVariable("KEEFI_DEV86_CAPTURE_DIR");if(!path.isEmpty()){QDir().mkpath(path);QSaveFile file(QDir(path).filePath("real-today-btc-three-lane.json"));QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(receipt).toJson());QVERIFY(file.commit());}
        QVERIFY2(result.state==State::Current,qPrintable(QString("Live public sources did not produce a current BTC premium: ")+result.reason));
    }
    void exactRequestAllowlist(){
        for(auto read:{Read::UpbitBtc,Read::BinanceBtc,Read::Fx}){const auto expected=TodayBtcPremium::request(read);QVERIFY(TodayBtcPremium::isRequest(expected));QVERIFY(NetworkTradingTransport::allowed(expected));
            auto r=expected;r.method="POST";QVERIFY(!TodayBtcPremium::isRequest(r));r=expected;r.authenticated=true;QVERIFY(!TodayBtcPremium::isRequest(r));
            r=expected;r.headers.insert("Authorization","secret");QVERIFY(!TodayBtcPremium::isRequest(r));r=expected;r.url.setHost("example.test");QVERIFY(!TodayBtcPremium::isRequest(r));
            r=expected;r.url.setQuery("symbol=BNBUSDT");QVERIFY(!TodayBtcPremium::isRequest(r));}
        // The ECB daily table is no longer read by anything: it left the allowlist.
        RequestSpec ecb;ecb.operation=Operation::Markets;ecb.url=QUrl("https://www.ecb.europa.eu/stats/eurofxref/eurofxref-daily.xml");ecb.headers.insert("Accept","application/xml");
        QVERIFY(!TodayBtcPremium::isRequest(ecb));QVERIFY(!NetworkTradingTransport::allowed(ecb));
    }
};
QTEST_GUILESS_MAIN(TodayBtcPremiumTest)
#include "tst_today_btc_premium.moc"
