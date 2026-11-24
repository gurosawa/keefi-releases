#include "keefi/PublicKrwPrices.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/TradingTransport.h"
#include "keefi/PublicQuoteStream.h"
#include "keefi/SolanaTransactionCodec.h"
#include <QPointer>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QTimeZone>
#include <QUrlQuery>
#include <algorithm>
#include <limits>
namespace keefi {
namespace {
QString stageName(PublicPriceStage stage) {
    if(stage==PublicPriceStage::BinanceCatalogue)return "Binance USDT 종목 목록";
    return stage==PublicPriceStage::Catalogue?QString("업비트 종목 목록"):QString("업비트 현재가");
}
QString reason(const PublicPriceReport& r) {
    switch(r.failure) {
    case PublicPriceFailure::RateLimited:return r.localThrottle?QString("같은 서비스의 제한 대기 · 이번 요청은 미전송"):QString("요청 제한 (HTTP %1)").arg(r.httpStatus);
    case PublicPriceFailure::Timeout:return "네트워크 시간 초과";
    case PublicPriceFailure::Network:return "네트워크 연결 오류";
    case PublicPriceFailure::Http:return QString("서버 응답 오류 (HTTP %1)").arg(r.httpStatus);
    case PublicPriceFailure::Json:return "응답 JSON 형식 오류";
    case PublicPriceFailure::Catalogue:return "공식 종목 목록 형식 오류";
    case PublicPriceFailure::Quote:return "가격·종목·원본 시각의 응답 범위 오류";
    case PublicPriceFailure::Missing:return "요청한 가격 일부 누락";
    case PublicPriceFailure::None:return {};
    }
    return {};
}
QString nextAction(const PublicPriceReport& r) {
    switch(r.failure) {
    case PublicPriceFailure::RateLimited:return "표시된 대기 시간이 지난 뒤 원화 평가가격을 다시 조회하세요.";
    case PublicPriceFailure::Timeout:case PublicPriceFailure::Network:return "인터넷 연결을 확인한 뒤 원화 평가가격을 다시 조회하세요.";
    case PublicPriceFailure::Http:return "잠시 후 다시 조회하세요. 계속되면 제공처·단계·HTTP 상태만 알려 주세요.";
    case PublicPriceFailure::Json:case PublicPriceFailure::Catalogue:case PublicPriceFailure::Quote:case PublicPriceFailure::Missing:
        return "가격 응답을 확인하지 못했습니다. 다시 조회해도 같으면 이 오류 종류를 알려 주세요.";
    case PublicPriceFailure::None:return r.stale?QString("180초보다 오래된 공식 가격은 기준 시각과 함께 참고 평가로 표시하며 합계에서 제외합니다. 가격 갱신이 확인되면 정상 평가로 전환합니다."):
        (r.unsupported||r.unmapped)?QString("공식 원화·USDT 평가 경로가 없거나 지원하지 않는 자산 식별자는 미평가로 남습니다."):QString{};
    }
    return {};
}
}
struct PublicKrwPrices::Private {
    PublicKrwPrices* q;TradingTransport* transport;QTimer timer,activation;QElapsedTimer clock;
    QSet<QString> wanted,cycle,markets;QStringList remaining,requested;
    QList<AssetQuote> updates;QString message;PublicPriceReport report;quint64 token=0;Operation operation=Operation::Markets;
    QMap<QString,AssetQuote> references;
    QMap<QString,AssetQuote> publicCache;
    QMetaObject::Connection catalogConnection;
    quint64 catalogRevision = 0;
    struct DeferredPrice { QMap<qint64,AssetQuote> candidates;qint64 conflictedThrough=0; };
    QMap<QString,DeferredPrice> deferred;
    static constexpr qsizetype MaxDeferredMarkets=2000;
    quint64 activationGeneration=0;
    QPointer<PublicQuoteStream> binanceStream;
    QSet<QString> binanceMarkets,binanceWanted;
    qint64 binanceCatalogueAt=-300001;
    bool binanceCatalogue=false;
    bool busy=false,catalogue=false,pending=false;int unqueryable=0,cycleUnqueryable=0;
    qint64 catalogueAt=-300001,nextSend=0,cooldown=0;
    Private(PublicKrwPrices* owner,TradingTransport* t):q(owner),transport(t){clock.start();timer.setSingleShot(true);activation.setSingleShot(true);activation.setTimerType(Qt::PreciseTimer);}
    void cancel(){++token;++activationGeneration;timer.stop();activation.stop();deferred.clear();transport->abort();busy=false;pending=false;wanted.clear();cycle.clear();remaining.clear();requested.clear();updates.clear();references.clear();unqueryable=cycleUnqueryable=0;binanceWanted.clear();if(binanceStream)binanceStream->setMarkets({});}
    bool freshQuote(const AssetQuote& quote)const{const auto now=QDateTime::currentMSecsSinceEpoch();const auto& e=quote.evidence;
        return e.complete&&e.sourceTimeMs>0&&e.sourceTimeMs<=now&&now-e.sourceTimeMs<=DisplayFreshnessMs&&e.receivedAtMs>0&&e.receivedAtMs<=now&&now-e.receivedAtMs<=DisplayFreshnessMs;}
    void scheduleActivation(){
        if(deferred.isEmpty()){activation.stop();return;}
        qint64 earliest=std::numeric_limits<qint64>::max();
        for(const auto& state:deferred)if(!state.candidates.isEmpty())earliest=qMin(earliest,state.candidates.firstKey());
        if(earliest==std::numeric_limits<qint64>::max()){activation.stop();return;}
        activation.start(int(qBound<qint64>(qint64(1),earliest-QDateTime::currentMSecsSinceEpoch()+1,qint64(2001))));
    }
    bool deferFuture(const QString& key,const AssetQuote& quote){
        const auto now=QDateTime::currentMSecsSinceEpoch();const auto& e=quote.evidence;
        auto state=deferred.find(key);
        if(state!=deferred.end()&&e.sourceTimeMs<=state->conflictedThrough)return true;
        if(e.sourceTimeMs<=now)return false;
        // Streaming producers accept a small clock lead. Do not replace a
        // usable price until that original source time can actually be used.
        if(!e.complete||!decimal::positive(quote.price)||e.source.isEmpty()||e.url.isEmpty()||
            e.sourceTimeMs-now>2000||e.receivedAtMs<=0||e.receivedAtMs>now||e.sourceTimeMs-e.receivedAtMs>2000)return true;
        if(state==deferred.end()){
            if(deferred.size()>=MaxDeferredMarkets)return true;
            state=deferred.insert(key,{});
        }
        auto& candidates=state->candidates;const auto old=candidates.constFind(e.sourceTimeMs);
        if(old!=candidates.cend()){
            if(decimal::normalize(old->price)!=decimal::normalize(quote.price)){
                state->conflictedThrough=qMax(state->conflictedThrough,e.sourceTimeMs);
                candidates.remove(e.sourceTimeMs);
            }
            scheduleActivation();return true; // Duplicates never renew receipt time.
        }
        candidates[e.sourceTimeMs]=quote;
        // Preserve the first maturity deadline under a continuous stream, plus
        // the newest candidate. Both this queue and market count are bounded.
        while(candidates.size()>2){auto middle=candidates.begin();++middle;candidates.erase(middle);}
        scheduleActivation();return true;
    }
    void accepted(const QString& key,qint64 sourceTime){
        auto state=deferred.find(key);if(state==deferred.end())return;
        if(sourceTime>state->conflictedThrough)state->conflictedThrough=0;
        while(!state->candidates.isEmpty()&&state->candidates.firstKey()<=sourceTime)state->candidates.erase(state->candidates.begin());
        if(state->candidates.isEmpty()&&!state->conflictedThrough)deferred.erase(state);
    }
    void activate(){
        const auto generation=activationGeneration;const auto now=QDateTime::currentMSecsSinceEpoch();
        QList<AssetQuote> publicQuotes,binanceQuotes;
        for(auto state=deferred.begin();state!=deferred.end();){
            auto& candidates=state->candidates;
            while(!candidates.isEmpty()&&candidates.firstKey()<=now){
                const auto quote=candidates.first();candidates.erase(candidates.begin());
                if(quote.evidence.sourceTimeMs<=state->conflictedThrough||!freshQuote(quote))continue;
                const auto key=(quote.quoteAsset=="KRW"?"KRW-":"BINANCE-")+quote.baseAsset;
                const auto old=references.constFind(key);
                if(old!=references.cend()&&quote.evidence.sourceTimeMs<=old->evidence.sourceTimeMs)continue;
                if(quote.quoteAsset=="KRW")publicQuotes.append(quote);else binanceQuotes.append(quote);
            }
            if(candidates.isEmpty()&&!state->conflictedThrough)state=deferred.erase(state);else ++state;
        }
        if(!publicQuotes.isEmpty())q->setPublicQuotes(publicQuotes,{});
        if(generation!=activationGeneration)return; // A delivery may lock/reset the portfolio.
        QList<AssetQuote> delivered;
        for(const auto& quote:binanceQuotes){
            if(!binanceWanted.contains(quote.baseAsset))continue;
            const auto key="BINANCE-"+quote.baseAsset;const auto old=references.constFind(key);
            if(old!=references.cend()&&quote.evidence.sourceTimeMs<=old->evidence.sourceTimeMs)continue;
            references[key]=quote;accepted(key,quote.evidence.sourceTimeMs);delivered.append(quote);
        }
        if(!delivered.isEmpty()){countStreams();emit q->completed(delivered);emit q->changed();}
        if(generation==activationGeneration)scheduleActivation();
    }
    bool needsBinance()const{return binanceStream&&std::any_of(cycle.cbegin(),cycle.cend(),[](const QString& market){return market!="KRW-USDT";});}
    void configureBinance(){
        if(!binanceStream)return;QStringList codes;binanceWanted.clear();
        // Keep eligible held-asset routes warm so an idle KRW market can expire
        // without waiting for another account refresh or socket connection.
        for(const auto& market:cycle)if(market!="KRW-USDT"&&binanceMarkets.contains(market.mid(4)+"USDT")){codes.append(market.mid(4)+"USDT");binanceWanted.insert(market.mid(4));}
        binanceStream->setMarkets(codes);report.unsupported=0;
        for(const auto& market:cycle)if(!markets.contains(market)&&!binanceWanted.contains(market.mid(4)))++report.unsupported;
    }
    void countStreams(){
        if(!binanceStream)return;report.streamPending=0;
        for(const auto& asset:binanceWanted)if(!freshQuote(references.value("KRW-"+asset))&&
            (!freshQuote(references.value("BINANCE-"+asset))||!freshQuote(references.value("KRW-USDT"))))++report.streamPending;
    }
    void begin(){
        if(clock.elapsed()<cooldown){message=QString("평가가격 조회 제한 대기 · %1초 후 다시 조회할 수 있습니다.").arg((cooldown-clock.elapsed()+999)/1000);emit q->changed();return;}
        cycle=wanted;cycleUnqueryable=unqueryable;pending=false;report={};report.requested=cycle.size()+cycleUnqueryable;
        for(auto it=references.begin();it!=references.end();)if(!cycle.contains("KRW-"+it->baseAsset)&&it->baseAsset!="USDT")it=references.erase(it);else ++it;
        report.unmapped=cycleUnqueryable;updates.clear();remaining.clear();requested.clear();
        if(cycle.isEmpty()){binanceWanted.clear();if(binanceStream)binanceStream->setMarkets({});finish();return;}
        busy=true;binanceCatalogue=false;catalogue=clock.elapsed()-catalogueAt>300000;
        if(!catalogue)prepare();report.stage=catalogue?PublicPriceStage::Catalogue:PublicPriceStage::Quotes;
        message="공개 원화 평가가격 조회 중 · 계좌 조회와 별도 처리";emit q->changed();timer.start(0);
    }
    void prepare(){
        remaining.clear();int direct=0;
        auto add=[this](const QString& market){const auto quote=publicCache.value(market);
            if(freshQuote(quote)){references[market]=quote;updates.append(quote);}else remaining.append(market);};
        for(const auto& market:cycle)if(markets.contains(market)){++direct;add(market);}
        report.catalogueKnown=true;report.unsupported=cycle.size()-direct;
        if(needsBinance()&&markets.contains("KRW-USDT")&&!cycle.contains("KRW-USDT"))add("KRW-USDT");
        remaining.sort();
        if(binanceStream&&clock.elapsed()-binanceCatalogueAt<=300000)configureBinance();
        // A usable public cache paints immediately, before a slow or missing market completes.
        if(!updates.isEmpty())emit q->completed(updates);
    }
    void finish(PublicPriceFailure failure=PublicPriceFailure::None,int httpStatus=0){
        busy=false;report.failure=failure;report.httpStatus=httpStatus;report.updated=0;
        for(const auto& quote:updates)if(cycle.contains("KRW-"+quote.baseAsset))++report.updated;
        countStreams();
        report.failed=failure==PublicPriceFailure::None?0:qMax(0,report.requested-report.updated-report.unsupported-report.unmapped-report.stale);
        if(failure!=PublicPriceFailure::None){
            message=(updates.isEmpty()?QString("평가가격 조회 실패"):QString("평가가격 부분 실패"))+" · "+stageName(report.stage)+" · "+reason(report);
            if(failure==PublicPriceFailure::RateLimited)message+=QString(" · %1초 후 재조회 가능").arg(qMax<qint64>(0,(cooldown-clock.elapsed()+999)/1000));
        }else if(report.stale){
            message=QString("평가가격 부분 완료 · 최신 %1개 / 참고 가격 %2개 (합계 제외)").arg(report.updated).arg(report.stale);
            if(report.unsupported||report.unmapped)message+=QString(" / 미지원 %1개").arg(report.unsupported+report.unmapped);
        }
        else if(report.unsupported||report.unmapped){
            message=QString(report.updated?"평가가격 부분 완료 · 갱신 %1개 / 평가 경로 없음 %2개":"원화 평가 미지원 · 갱신 %1개 / 평가 경로 없음 %2개").arg(report.updated).arg(report.unsupported);
            if(report.unmapped)message+=QString(" / 식별자 미지원 %1개").arg(report.unmapped);
        }
        else if(!report.requested)message="평가가격 조회 대상 없음 · 0 잔고·원화는 요청 생략";
        else if(report.streamPending)message=QString("직접 원화 가격 %1개 · Binance 경유 가격 %2개 수신 중").arg(report.updated).arg(report.streamPending);
        else message=QString("평가가격 조회 완료 · %1개 갱신").arg(report.updated);
        if(binanceStream&&!binanceWanted.isEmpty())message+=QString(" · Binance USDT 경유 %1종 · 시세 자동 수신").arg(binanceWanted.size());
        const auto completed=updates;updates.clear();const auto generation=token;const bool again=pending&&failure==PublicPriceFailure::None;pending=false;
        emit q->changed();emit q->completed(completed);
        if(again&&generation==token&&!busy)begin();
    }
    void next(){
        if(!busy)return;
        if(const auto wait=nextSend-clock.elapsed();wait>0){timer.start(int(wait));return;} // One clock read; never a negative wait.
        RequestSpec r;
        if(catalogue){operation=Operation::Markets;report.stage=PublicPriceStage::Catalogue;r.url=QUrl("https://api.upbit.com/v1/market/all");}
        else {
            if(remaining.isEmpty()){
                if(needsBinance()&&clock.elapsed()-binanceCatalogueAt>300000){
                    binanceCatalogue=true;operation=Operation::Markets;report.stage=PublicPriceStage::BinanceCatalogue;
                    r.url=QUrl("https://api.binance.com/api/v3/exchangeInfo");QUrlQuery query;query.addQueryItem("permissions","SPOT");query.addQueryItem("showPermissionSets","false");r.url.setQuery(query);
                    r.operation=operation;nextSend=clock.elapsed()+350;transport->send(++token,r);return;
                }
                finish();return;
            }
            requested=remaining.mid(0,100);remaining=remaining.mid(requested.size());operation=Operation::Quote;report.stage=PublicPriceStage::Quotes;
            r.url=QUrl("https://api.upbit.com/v1/ticker");QUrlQuery query;query.addQueryItem("markets",requested.join(','));r.url.setQuery(query);
        }
        r.operation=operation;nextSend=clock.elapsed()+350;transport->send(++token,r);
    }
    void result(quint64 generation,const HttpResult& response){
        if(generation!=token||!busy)return;
        if(response.status==429||response.status==418){
            report.localThrottle=response.localRetryAfterSeconds>0;
            qint64 seconds=report.localThrottle?response.localRetryAfterSeconds:60;
            for(auto it=response.headers.cbegin();it!=response.headers.cend();++it)if(it.key().compare("Retry-After",Qt::CaseInsensitive)==0){bool ok=false;const auto n=it.value().toLongLong(&ok);if(ok&&n>0)seconds=qMax(seconds,qMin<qint64>(n,86400));}
            cooldown=clock.elapsed()+seconds*1000;report.retryAtMs=QDateTime::currentMSecsSinceEpoch()+seconds*1000;finish(PublicPriceFailure::RateLimited,response.status);return;
        }
        if(response.timedOut){finish(PublicPriceFailure::Timeout);return;}
        if(response.networkError){finish(PublicPriceFailure::Network);return;}
        if(response.status!=200){finish(PublicPriceFailure::Http,response.status);return;}
        const auto document=parseExactJson(response.body,nullptr,binanceCatalogue?JsonResponseScope::BinanceMarketCatalog:JsonResponseScope::Standard);
        if(binanceCatalogue){
            const auto symbols=document.object().value("symbols");
            if(!document.isObject()||!symbols.isArray()||symbols.toArray().size()>20000){finish(PublicPriceFailure::Catalogue);return;}
            QSet<QString> available,seen;static const QRegularExpression asset("\\A[A-Z0-9]{1,24}\\z");
            for(const auto& value:symbols.toArray()){const auto row=value.toObject();const auto symbol=row.value("symbol").toString(),base=row.value("baseAsset").toString(),quote=row.value("quoteAsset").toString();
                if(!value.isObject()||symbol.isEmpty()||seen.contains(symbol)){finish(PublicPriceFailure::Catalogue);return;}seen.insert(symbol);
                if(row.value("status").toString()=="TRADING"&&row.value("isSpotTradingAllowed").toBool()&&quote=="USDT"&&asset.match(base).hasMatch()&&symbol==base+quote)available.insert(symbol);
            }
            binanceMarkets=available;binanceCatalogueAt=clock.elapsed();binanceCatalogue=false;configureBinance();finish();return;
        }
        if(!document.isArray()){finish(PublicPriceFailure::Json);return;}
        if(catalogue){
            if(document.array().size()>20000){finish(PublicPriceFailure::Catalogue);return;}
            QSet<QString> available,seen;
            for(const auto& v:document.array()){
                const auto market=v.toObject().value("market").toString();
                if(!v.isObject()||market.isEmpty()||seen.contains(market)){finish(PublicPriceFailure::Catalogue);return;}seen.insert(market);
                if(QRegularExpression("\\AKRW-[A-Z][A-Z0-9]{0,23}\\z").match(market).hasMatch())available.insert(market);
            }
            markets=available;catalogueAt=clock.elapsed();catalogue=false;prepare();timer.start(350);return;
        }
        QSet<QString> seen;QList<AssetQuote> page;QMap<QString,AssetQuote> validatedPage;int stalePage=0;const auto now=QDateTime::currentMSecsSinceEpoch();
        for(const auto& v:document.array()){
            const auto row=v.toObject();const auto market=row.value("market").toString(),price=row.value("trade_price").toString();bool ok=false;const auto time=row.value("timestamp").toString().toLongLong(&ok);
            if(!v.isObject()||!requested.contains(market)||seen.contains(market)||!decimal::positive(price)||!ok||time<=0||time-now>2000){finish(PublicPriceFailure::Quote);return;}
            seen.insert(market);
            const AssetQuote quote{market.mid(4),"KRW",price,{"업비트 공식 KRW 현재가","https://api.upbit.com/v1/ticker","public:upbit:"+market,time,now,true}};
            validatedPage.insert(market,quote);if(now-time>DisplayFreshnessMs)++stalePage;else page.append(quote);
        }
        if(seen.size()!=requested.size()){finish(PublicPriceFailure::Missing);return;}
        // Only commit a completely validated page. Keep stale prices outside the
        // current-price cache and retain their original timestamps verbatim.
        bool conflict=false;
        for(auto it=validatedPage.cbegin();it!=validatedPage.cend();++it){auto previous=references.find(it.key());
            if(previous!=references.end()&&it->evidence.sourceTimeMs<=previous->evidence.sourceTimeMs&&
                (!previous->evidence.complete||(it->evidence.sourceTimeMs==previous->evidence.sourceTimeMs&&
                    decimal::normalize(it->price)!=decimal::normalize(previous->price)))){
                // A tombstone prevents the next retry from reviving either value.
                previous->evidence.complete=false;conflict=true;
            }
        }
        if(conflict){finish(PublicPriceFailure::Quote);return;}
        for(auto it=validatedPage.cbegin();it!=validatedPage.cend();++it)
            if(!references.contains(it.key())||it->evidence.sourceTimeMs>references.value(it.key()).evidence.sourceTimeMs)
                references[it.key()]=it.value();
        // Retain fresh candidates too: after they age, a failed refresh must not
        // erase the last official reference or renew its source/receipt times.
        updates.append(page);report.stale+=stalePage;emit q->completed(page);timer.start(0);
    }
};
PublicKrwPrices::PublicKrwPrices(TradingTransport* transport,QObject* parent):QObject(parent),d_(std::make_unique<Private>(this,transport)){
    connect(&d_->timer,&QTimer::timeout,this,[this]{d_->next();});
    connect(&d_->activation,&QTimer::timeout,this,[this]{d_->activate();});
    connect(transport,&TradingTransport::finished,this,[this](quint64 n,const HttpResult& r){d_->result(n,r);});
}
PublicKrwPrices::~PublicKrwPrices(){d_->cancel();}
void PublicKrwPrices::setPublicMarketCatalog(PublicMarketCatalog* catalog) {
    disconnect(d_->catalogConnection);
    d_->catalogRevision = 0;
    if (!catalog) return;
    const auto sync = [this, catalog] {
        const auto codes = catalog->marketCodes();
        if (!codes.isEmpty()) {
            d_->markets = codes;
            d_->catalogueAt = d_->clock.elapsed();
        }
        setPublicQuotes(catalog->quotesSince(d_->catalogRevision), {});
    };
    d_->catalogConnection = connect(catalog, &PublicMarketCatalog::changed, this, sync);
    sync();
}
void PublicKrwPrices::setPublicQuotes(const QList<AssetQuote>& quotes,const QStringList& markets){
    if(!markets.isEmpty()){d_->markets=QSet<QString>(markets.cbegin(),markets.cend());d_->catalogueAt=d_->clock.elapsed();}
    QList<AssetQuote> relevant;
    for(const auto& quote:quotes){
        const auto market="KRW-"+quote.baseAsset;
        const auto old=d_->publicCache.constFind(market);if(old!=d_->publicCache.cend()&&quote.evidence.sourceTimeMs<=old->evidence.sourceTimeMs)continue;
        if(quote.quoteAsset!="KRW"||quote.evidence.scope!="public:upbit:"+market||!quote.evidence.complete||!decimal::positive(quote.price))continue;
        if(d_->deferFuture(market,quote)){
            // A later catalogue notification can make this price usable, including after clear().
            d_->catalogRevision=0;
            continue;
        }
        d_->publicCache[market]=quote;
        d_->accepted(market,quote.evidence.sourceTimeMs);
        if(d_->wanted.contains(market)||(!d_->binanceWanted.isEmpty()&&quote.baseAsset=="USDT")){
            d_->references[market]=quote;if(d_->freshQuote(quote))relevant.append(quote);
        }
    }
    if(!relevant.isEmpty()){d_->countStreams();emit completed(relevant);emit changed();}
}
void PublicKrwPrices::setBinanceStream(PublicQuoteStream* stream){
    if(d_->binanceStream){d_->binanceStream->setMarkets({});disconnect(d_->binanceStream,nullptr,this,nullptr);}d_->binanceStream=stream;if(!stream)return;
    connect(stream,&PublicQuoteStream::changed,this,&PublicKrwPrices::changed);
    connect(stream,&PublicQuoteStream::ticks,this,[this](const QList<PublicTick>& ticks){QList<AssetQuote> updated;
        for(const auto& tick:ticks){const auto& quote=tick.quote;
            if(!d_->binanceWanted.contains(quote.baseAsset)||quote.quoteAsset!="USDT"||quote.evidence.scope!="public:binance:"+tick.market)continue;
            if(d_->deferFuture("BINANCE-"+quote.baseAsset,quote))continue;
            d_->references["BINANCE-"+quote.baseAsset]=quote;d_->accepted("BINANCE-"+quote.baseAsset,quote.evidence.sourceTimeMs);if(d_->freshQuote(quote))updated.append(quote);
        }
        if(!updated.isEmpty()){d_->countStreams();if(!d_->report.streamPending)d_->message="평가가격 자동 갱신 · 업비트 KRW와 Binance USDT 경유";emit completed(updated);emit changed();}
    });
}
void PublicKrwPrices::refresh(const QList<AssetSnapshot>& snapshots){
    QSet<QString> wanted,unqueryable;
    for(const auto& s:snapshots)for(const auto& c:s.components){
        if(c.kind==AssetComponentKind::PositionNotional)continue;
        const auto amount=c.reportedValue.isEmpty()?c.quantity:c.reportedValue;
        const auto absolute=amount.startsWith('-')?amount.mid(1):amount;
        if(decimal::valid(absolute)&&decimal::normalize(absolute)=="0")continue;
        const auto asset=c.reportedValue.isEmpty()?c.asset:c.reportedValueCurrency;
        if(asset=="KRW")continue;
        if(s.market=="solana"&&asset.startsWith("solana:")&&SolanaTransactionCodec::publicKeyValid(asset.mid(7))){
            wanted.insert("KRW-USDC");continue; // Mint USD prices arrive through the wallet read path.
        }
        if(QRegularExpression("\\A[A-Z][A-Z0-9]{0,23}\\z").match(asset).hasMatch())wanted.insert("KRW-"+asset);
        else unqueryable.insert(asset);
    }
    d_->wanted=wanted;d_->unqueryable=unqueryable.size();if(d_->busy){d_->pending=d_->wanted!=d_->cycle||d_->unqueryable!=d_->cycleUnqueryable;return;}d_->begin();
}
void PublicKrwPrices::clear(){d_->cancel();d_->report={};d_->message.clear();emit changed();}
bool PublicKrwPrices::busy()const{return d_->busy;}
QString PublicKrwPrices::status()const{return d_->message;}
PublicPriceReport PublicKrwPrices::report()const{return d_->report;}
QList<AssetQuote> PublicKrwPrices::referenceQuotes()const{
    QList<AssetQuote> result;const auto now=QDateTime::currentMSecsSinceEpoch();
    for(const auto& quote:d_->references)if(quote.evidence.complete&&
        (now-quote.evidence.sourceTimeMs>DisplayFreshnessMs||now-quote.evidence.receivedAtMs>DisplayFreshnessMs))result.append(quote);
    return result;
}
QString PublicKrwPrices::details()const{
    if(d_->message.isEmpty())return {};
    const auto& r=d_->report;QStringList lines{"원화 평가가격: 최신 업비트 KRW 우선 · 없거나 오래되면 Binance USDT × 업비트 KRW-USDT · 금융 계좌 인증과 별도"};
    if(d_->binanceStream&&!d_->binanceStream->status().isEmpty())lines<<d_->binanceStream->status();
    if(r.stage!=PublicPriceStage::None)lines<<"조회 단계: "+stageName(r.stage);
    if(r.failure!=PublicPriceFailure::None)lines<<"원인: "+reason(r);
    lines<<QString("대상 %1개 · 직접 가격 갱신 %2개 · 평가 경로 없음 %3개 · 식별자 미지원 %4개 · 오래된 가격 %5개 · 조회 실패 %6개 · 스트림 수신 중 %7개").arg(r.requested).arg(r.updated).arg(r.unsupported).arg(r.unmapped).arg(r.stale).arg(r.failed).arg(r.streamPending);
    if(!r.catalogueKnown&&r.requested>r.unmapped)lines<<"종목 목록을 확인하지 못했거나 조회 중입니다. 원화 마켓 지원 여부는 미확인입니다.";
    if(r.retryAtMs>0)lines<<"다시 조회 가능한 시각: "+QDateTime::fromMSecsSinceEpoch(r.retryAtMs).toTimeZone(QTimeZone("Asia/Seoul")).toString("HH:mm:ss")+" KST";
    const auto action=nextAction(r);if(!action.isEmpty())lines<<action;
    lines<<"오래된 공식 가격은 원래 기준 시각의 참고 평가이며 합계에서 제외합니다. 가격이 없거나 유효하지 않으면 미평가로 남습니다. 실패를 0으로 채우지 않습니다.";
    return lines.join('\n');
}
}
