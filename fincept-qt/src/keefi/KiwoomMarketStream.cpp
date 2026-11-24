#include "keefi/KiwoomMarketStream.h"
#include "keefi/BrokerTradingController.h"
#include "keefi/KiwoomRequestBudget.h"
#include "keefi/TradingTransport.h"
#include <QDateTime>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QLocale>
#include <QPointer>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSet>
#include <QTimer>
#include <QTimeZone>
#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>
namespace keefi {
namespace {
qint64 add(qint64 at,qint64 duration){return duration>std::numeric_limits<qint64>::max()-at?std::numeric_limits<qint64>::max():at+qMax<qint64>(0,duration);}
QString identityKey(const WatchlistIdentity& i){return i.market+':'+i.source+':'+i.code;}
QString key(const KiwoomMarketTarget& t){return identityKey(t.identity)+':'+t.exchange;}
QString key(const KiwoomVerifiedInstrument& i){return identityKey(i.identity)+':'+i.exchange;}
bool targetValid(const KiwoomMarketTarget& t){return t.identity.market=="stock"&&t.priority>=0&&t.priority<=3&&
    ((t.identity.source=="KR"&&QRegularExpression("\\A[A-Z0-9]{6}\\z").match(t.identity.code).hasMatch()&&(t.exchange.isEmpty()||t.exchange=="KRX"))||
    (t.identity.source=="US"&&QRegularExpression("\\A[A-Z]{1,8}(?:[.-][A-Z])?\\z").match(t.identity.code).hasMatch()&&(t.exchange.isEmpty()||QStringList{"ND","NY","NA"}.contains(t.exchange))));}
bool officialUrl(const QString& text,const QString& path){const QUrl url(text);return url.isValid()&&url.scheme()=="https"&&url.host()=="api.kiwoom.com"&&url.port(443)==443&&url.userInfo().isEmpty()&&!url.hasQuery()&&!url.hasFragment()&&url.path()==path;}
bool fresh(const AssetEvidence& e,qint64 at,qint64 ttl){return e.complete&&e.sourceTimeMs==0&&e.receivedAtMs>0&&e.receivedAtMs<=at&&at-e.receivedAtMs<ttl;}
bool metadataValid(const KiwoomVerifiedInstrument& i,qint64 at,bool requireCurrency){
    const bool us=i.identity.source=="US";if(!targetValid({i.identity,i.exchange,0,false})||i.exchange.isEmpty()||!fresh(i.metadataEvidence,at,KiwoomMarketStream::MetadataFreshnessMs)||
        !officialUrl(i.metadataEvidence.url,us?"/api/us/stkinfo":"/api/dostk/stkinfo")||!(us?QStringList{"usa10098","usa10099"}:QStringList{"ka10100","ka10099"}).contains(i.metadataApiId))return false;
    return !requireCurrency||(i.verified&&i.currency==(us?"USD":"KRW")&&i.currencyApiId==(us?"usa20100":"ka10001")&&fresh(i.currencyEvidence,at,KiwoomMarketStream::MetadataFreshnessMs)&&officialUrl(i.currencyEvidence.url,us?"/api/us/mrkcond":"/api/dostk/stkinfo"));
}
QString jobKey(const KiwoomReadIntent& i){return QString::number(int(i.read))+':'+i.country+':'+i.symbol+':'+i.exchange+':'+i.catalogMarket+':'+i.baseDate;}
qint64 retryDelay(const HttpResult& r,qint64 utc,bool business){
    qint64 delay=business?1000:60000;if(r.localRetryAfterSeconds>0)delay=qint64(r.localRetryAfterSeconds)*1000;
    for(auto it=r.headers.cbegin();it!=r.headers.cend();++it)if(it.key().compare("Retry-After",Qt::CaseInsensitive)==0){bool ok=false;const auto seconds=it.value().trimmed().toLongLong(&ok);
        if(ok&&seconds>0)delay=qMax(delay,seconds>std::numeric_limits<qint64>::max()/1000?std::numeric_limits<qint64>::max():seconds*1000);
        else{const auto text=QString::fromLatin1(it.value());auto date=QDateTime::fromString(text,Qt::RFC2822Date);static const QRegularExpression pattern("\\A(?:Mon|Tue|Wed|Thu|Fri|Sat|Sun), [0-9]{2} (?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2} GMT\\z");if(pattern.match(text).hasMatch()){const auto parsed=QLocale::c().toDateTime(text,"ddd, dd MMM yyyy HH:mm:ss 'GMT'");if(parsed.isValid())date=QDateTime(parsed.date(),parsed.time(),QTimeZone::UTC);}if(date.isValid()&&date.toMSecsSinceEpoch()>utc)delay=qMax(delay,date.toMSecsSinceEpoch()-utc);}}
    return delay;
}
void wipe(RequestSpec& r){for(auto it=r.headers.begin();it!=r.headers.end();++it)it.value().fill('\0');r.headers.clear();r.body.fill('\0');r.body.clear();}
}
struct KiwoomMarketStream::Private {
    enum class Phase{Idle,Opening,Login,Ready,Registering,Live,Closing};
    struct Job {KiwoomReadIntent intent;int pages=1;QSet<QByteArray> cursors;QList<KiwoomVerifiedInstrument> staged;qint64 firstReceipt=0;quint64 reconcileWire=0;};
    KiwoomMarketStream* q;QPointer<BrokerTradingController> broker;QPointer<TradingTransport> rest;std::unique_ptr<KiwoomMarketSocket> socket;std::shared_ptr<KiwoomRequestBudget> budget;
    Clock monotonic,utcClock;QTimer timer;QMetaObject::Connection brokerChanged,brokerRevoked,brokerDestroyed;
    QList<KiwoomMarketTarget> wanted,selected,restOnly,declared;QMap<QString,QList<KiwoomVerifiedInstrument>> catalogue;QMap<QString,KiwoomMarketQuote> values;QMap<QString,QByteArray> replay;QMap<QString,qint64> attempted;
    WatchlistIdentity dailyTarget;KiwoomDailySeries dailySeries;
    QList<Job> jobs;QSet<QString> queued;std::optional<Job> activeJob;quint64 httpToken=0,httpEpoch=0,sentRevision=0,lease=0,intentEpoch=1;
    Phase phase=Phase::Idle;QString activeCountry,wireCountry,message="기존 키움 인증이 필요합니다.";quint64 counter=0,wire=0,closingWire=0,revision=0;
    struct SessionHistory {QPointer<BrokerTradingController> owner;QMap<quint64,qint64> deadlines;QSet<quint64> blocked;};QList<SessionHistory> history;
    QSet<QString> confirmed,reconciled;QMap<quint64,qint64> deadlines;QSet<quint64> blocked;bool authDirty=true,pumping=false,notificationQueued=false,clockSeen=false;
    qint64 lastClock=0,lastUtc=0,phaseAt=0,lastHeard=0,httpAt=0,nextDispatch=0,nextOpen=0;int attempts=0;
    qint64 now()const{return monotonic();}qint64 utc()const{return utcClock();}
    void notify(){if(notificationQueued)return;notificationQueued=true;QTimer::singleShot(0,q,[this]{notificationQueued=false;emit q->changed();});}
    void status(const QString& s){if(message!=s){message=s;notify();}}
    bool clockValid(){const auto n=now(),u=utc();const bool valid=n>=0&&u>0&&(!clockSeen||(n>=lastClock&&u>=lastUtc));lastClock=clockSeen?qMax(lastClock,n):n;lastUtc=clockSeen?qMax(lastUtc,u):u;clockSeen=true;return valid;}
    bool kiwoom()const{return broker&&broker->policy().provider=="kiwoom";}
    bool session()const{return kiwoom()&&broker->authenticated()&&(!revision||(!blocked.contains(revision)&&(!deadlines.contains(revision)||now()<deadlines.value(revision))));}
    void rememberSession(){
        history.erase(std::remove_if(history.begin(),history.end(),[](const auto& h){return h.owner.isNull();}),history.end());if(!broker)return;
        for(auto& h:history)if(h.owner==broker){h.deadlines=deadlines;h.blocked=blocked;return;}history.append({broker,deadlines,blocked});
    }
    bool rememberOwner(BrokerTradingController* owner){
        history.erase(std::remove_if(history.begin(),history.end(),[](const auto& h){return h.owner.isNull();}),history.end());for(const auto& h:history)if(h.owner==owner)return true;if(history.size()>=128)return false;history.append({owner,{},{}});return true;
    }
    void restoreSession(BrokerTradingController* owner){deadlines.clear();blocked.clear();for(const auto& h:history)if(h.owner==owner&&owner){deadlines=h.deadlines;blocked=h.blocked;break;}}
    void release(qint64 pause=0){const auto owned=lease;lease=0;if(owned)budget->finish(owned,pause);}
    void cancelHttp(){
        ++httpToken;activeJob.reset();queued.clear();jobs.clear();
        // Invalidate local ownership before a synchronous abort callback, but
        // retain the global in-flight reservation until physical abort returns.
        const auto owned=std::exchange(lease,quint64(0));const auto sharedBudget=budget;const auto transport=rest;
        if(transport)transport->abort();if(owned)sharedBudget->finish(owned);
    }
    void invalidateWire(){const auto old=wire?wire:closingWire;wire=closingWire=0;phase=Phase::Idle;wireCountry.clear();confirmed.clear();reconciled.clear();declared.clear();if(socket&&old)socket->abort();for(auto it=values.begin();it!=values.end();++it)it.value().realtime=false;}
    void revoke(bool eraseIntent){++intentEpoch;cancelHttp();invalidateWire();values.clear();catalogue.clear();attempted.clear();selected.clear();restOnly.clear();dailySeries={};if(eraseIntent){wanted.clear();dailyTarget={};activeCountry.clear();replay.clear();timer.stop();}revision=0;notify();}
    void blockCurrent(const QString& text){if(revision)blocked.insert(revision);authDirty=false;cancelHttp();invalidateWire();values.clear();catalogue.clear();selected.clear();restOnly.clear();status(text);notify();}
    bool auth(const std::function<void(QByteArrayView,quint64)>& callback){
        if(!kiwoom()||!broker->authenticated())return false;const auto epoch=intentEpoch;bool accepted=false;
        broker->withMarketAuthorization([&](QByteArrayView token,qint64 remaining,quint64 rev){
            if(epoch!=intentEpoch||remaining<=0||!rev)return;if(blocked.contains(rev)){revision=rev;return;}if(!deadlines.contains(rev)&&deadlines.size()>=128)return;const auto deadline=add(now(),remaining);if(deadlines.contains(rev))deadlines[rev]=qMin(deadlines.value(rev),deadline);else deadlines[rev]=deadline;
            if(now()>=deadlines.value(rev)){blocked.insert(rev);return;}
            if(revision&&revision!=rev){cancelHttp();invalidateWire();values.clear();catalogue.clear();replay.clear();attempted.clear();}
            revision=rev;authDirty=false;accepted=true;callback(token,rev);
        });if(!accepted)authDirty=false;return accepted&&epoch==intentEpoch;
    }
    bool currentReply(quint64 rev){if(!session()||revision!=rev)return false;bool current=false;auth([&](QByteArrayView,quint64 actual){current=actual==rev;});return current&&revision==rev;}
    QList<KiwoomVerifiedInstrument> matching(const KiwoomMarketTarget& target,bool verified)const{
        QList<KiwoomVerifiedInstrument> out;for(const auto& i:catalogue.value(identityKey(target.identity)))if((target.exchange.isEmpty()||target.exchange==i.exchange)&&metadataValid(i,utc(),verified))out.append(i);return out;
    }
    std::optional<KiwoomVerifiedInstrument> resolve(const KiwoomMarketTarget& t,bool verified)const{const auto rows=matching(t,verified);if(rows.size()!=1)return {};return rows.first();}
    bool wantedIntent(const KiwoomReadIntent& i)const{if(i.read==KiwoomDisplayRequest::KrDaily)return dailyTarget.source=="KR"&&dailyTarget.code==i.symbol;for(const auto& t:wanted)if(t.identity.source==i.country&&t.identity.code==i.symbol&&(i.exchange.isEmpty()||t.exchange.isEmpty()||i.exchange==t.exchange))return true;return false;}
    void enqueue(Job job,bool force=false,bool front=false){const auto k=jobKey(job.intent);if(queued.contains(k)){if(job.reconcileWire)for(auto& pending:jobs)if(jobKey(pending.intent)==k)pending.reconcileWire=job.reconcileWire;return;}if((!force&&attempted.value(k,0)>now())||jobs.size()>=10000)return;queued.insert(k);if(front)jobs.prepend(std::move(job));else jobs.append(std::move(job));}
    Job metadataJob(const KiwoomMarketTarget& t)const{Job j;j.intent.country=t.identity.source;j.intent.symbol=t.identity.code;j.intent.exchange=t.identity.source=="US"?t.exchange:QStringLiteral("KRX");j.intent.read=t.identity.source=="US"?KiwoomDisplayRequest::UsMetadata:KiwoomDisplayRequest::KrMetadata;return j;}
    Job priceJob(const KiwoomVerifiedInstrument& i,quint64 w=0)const{Job j;j.intent.country=i.identity.source;j.intent.symbol=i.identity.code;j.intent.exchange=i.exchange;j.intent.read=i.identity.source=="US"?KiwoomDisplayRequest::UsPrice:KiwoomDisplayRequest::KrPrice;j.reconcileWire=w;return j;}
    void plan(){
        if(dailyTarget.source=="KR"&&activeCountry=="KR"&&(!dailySeries.available||dailySeries.identity!=dailyTarget||utc()-dailySeries.receivedAtMs>=60000)){
            Job daily;daily.intent.read=KiwoomDisplayRequest::KrDaily;daily.intent.country="KR";daily.intent.symbol=dailyTarget.code;
            daily.intent.baseDate=QDateTime::fromMSecsSinceEpoch(utc(),QTimeZone("Asia/Seoul")).date().toString("yyyyMMdd");enqueue(daily);
        }
        if(wanted.isEmpty()||activeCountry.isEmpty())return;
        for(const auto& t:wanted){const auto metadata=resolve(t,false);if(!metadata){if(matching(t,false).size()>1){status("키움 거래소 정보가 모호하여 해당 종목을 숨겼습니다.");continue;}enqueue(metadataJob(t));continue;}
            const auto i=resolve(t,true);if(!i){enqueue(priceJob(*metadata));continue;}const auto k=key(*i);
            if(confirmed.contains(k)&&!reconciled.contains(k))enqueue(priceJob(*i,wire),true);
            else {
                const bool onSocket=std::any_of(selected.cbegin(),selected.cend(),[&](const auto& selectedTarget){return key(selectedTarget)==k;});
                // A verified active WS target waits for REG ACK before its first REST
                // reconciliation; overflow/inactive-country rows use REST directly.
                const bool mayRead=!socket||!onSocket||(phase==Phase::Live&&reconciled.contains(k));
                if(mayRead&&(!values.contains(k)||!fresh(values.value(k).evidence,utc(),QuoteFreshnessMs)||attempted.value(jobKey(priceJob(*i).intent),0)<=now()))enqueue(priceJob(*i));
            }
        }
    }
    void repartition(){
        QList<KiwoomMarketTarget> live,overflow;QSet<QString> seen;for(const auto& target:wanted){auto resolved=resolve(target,true);if(!resolved)continue;auto t=target;t.exchange=resolved->exchange;const auto k=key(t);if(seen.contains(k))continue;seen.insert(k);if(t.identity.source==activeCountry&&live.size()<MaxRealtimeSymbols)live.append(t);else overflow.append(t);}
        if(live==selected&&overflow==restOnly)return;selected=live;restOnly=overflow;notify();
        QSet<QString> keep;for(const auto& t:selected)keep.insert(key(t));for(const auto& t:restOnly)keep.insert(key(t));for(auto it=values.begin();it!=values.end();)if(!keep.contains(it.key()))it=values.erase(it);else++it;
        if((phase==Phase::Ready||phase==Phase::Live)&&wireCountry==activeCountry)declare();
    }
    void closeForSelection(){
        if(phase==Phase::Closing)return;if(!wire){phase=Phase::Idle;return;}const auto old=wire;wire=0;closingWire=old;phase=Phase::Closing;phaseAt=now();
        const auto remove=KiwoomMarketProtocol::subscription(wireCountry,declared,true);if(socket&&!remove.isEmpty())socket->sendControl(old,remove);
        confirmed.clear();reconciled.clear();declared.clear();values.clear();cancelHttp();if(socket)socket->close(old);else{closingWire=0;phase=Phase::Idle;}notify();
    }
    void maybeOpen(){
        if(phase!=Phase::Idle||!socket||selected.isEmpty()||activeCountry.isEmpty()||now()<nextOpen||!kiwoom()||!broker->authenticated())return;
        if((!revision||blocked.contains(revision))&&!authDirty)return;
        // Opening an actual socket is a dispatch boundary too. Validate the
        // current borrowed revision before opening, including a restored history.
        if(!auth([](QByteArrayView,quint64){})){status("기존 키움 인증이 필요하거나 만료되었습니다.");return;}
        repartition();if(selected.isEmpty()||!session())return;
        if(counter==std::numeric_limits<quint64>::max())return;wire=++counter;wireCountry=activeCountry;phase=Phase::Opening;phaseAt=lastHeard=now();status("키움 실시간 연결 중 · 실연결 미검증");socket->open(wire,wireCountry);
    }
    void declare(){
        if(!wire||wireCountry!=activeCountry||(phase!=Phase::Ready&&phase!=Phase::Live)||selected.isEmpty()){if(selected.isEmpty()&&wire)closeForSelection();return;}
        const auto packet=KiwoomMarketProtocol::subscription(activeCountry,selected);if(packet.isEmpty())return;declared=selected;confirmed.clear();reconciled.clear();for(auto it=values.begin();it!=values.end();++it)it.value().realtime=false;phase=Phase::Registering;phaseAt=now();socket->sendControl(wire,packet);status("키움 구독 승인 대기 · 실연결 미검증");
    }
    void connected(quint64 g){
        if(!g||g!=wire||phase!=Phase::Opening||!session())return;const auto epoch=intentEpoch;const bool sent=auth([&](QByteArrayView token,quint64){if(g!=wire||epoch!=intentEpoch)return;phase=Phase::Login;phaseAt=now();socket->sendLogin(g,token);});
        if(!sent&&g==wire)blockCurrent("기존 키움 인증을 확인할 수 없어 연결을 중단했습니다.");
    }
    void disconnected(quint64 g){
        if(g&&g==closingWire){closingWire=0;phase=Phase::Idle;wireCountry.clear();tick();return;}
        if(!g||g!=wire)return;invalidateWire();values.clear();nextOpen=add(now(),1000);status("키움 연결 끊김 · REST 대사 후 재개 · 실연결 미검증");notify();
    }
    void failed(quint64 g,int statusCode){if(g!=wire||!g)return;if(statusCode==401||statusCode==403){blockCurrent("키움 인증 또는 허용 IP를 기존 연결에서 확인하세요.");return;}invalidateWire();values.clear();nextOpen=add(now(),qMin<qint64>(60000,1000LL<<qMin(attempts++,5)));status("키움 재연결 대기 · 실시간 표시 소거");notify();}
    void frame(quint64 g,const QByteArray& bytes){
        if(!g||g!=wire||bytes.size()>KiwoomMarketProtocol::MaxFrameBytes||!session())return;
        if(const auto ping=KiwoomMarketProtocol::pingEcho(bytes)){lastHeard=now();socket->sendControl(g,*ping);return;}
        const auto expected=phase==Phase::Login?QStringLiteral("LOGIN"):phase==Phase::Registering?QStringLiteral("REG"):QString();
        if(!expected.isEmpty()){const auto ack=KiwoomMarketProtocol::controlAck(bytes,expected);if(ack.available){lastHeard=now();if(!ack.accepted){if(ack.authenticationFailed)blockCurrent("키움 기존 인증이 거부되었습니다. 직접 다시 인증하세요.");else failed(g,0);return;}
                if(expected=="LOGIN"){phase=Phase::Ready;declare();return;}phase=Phase::Live;attempts=0;confirmed.clear();for(const auto& t:declared)confirmed.insert(key(t));reconciled.clear();if(declared!=selected){phase=Phase::Ready;declare();return;}for(auto it=selected.crbegin();it!=selected.crend();++it)if(const auto i=resolve(*it,true))enqueue(priceJob(*i,wire),true,true);plan();dispatch();status("키움 현재가 REST 대사 중 · 원천 UTC 시각 미제공");return;}}
        if(phase!=Phase::Live)return;QList<KiwoomVerifiedInstrument> proof;for(const auto& t:selected)if(confirmed.contains(key(t))&&reconciled.contains(key(t))){const auto i=resolve(t,true);if(i)proof.append(*i);}
        const auto rows=KiwoomMarketProtocol::realtimeQuotes(bytes,wireCountry,proof,utc());if(rows.isEmpty())return;lastHeard=now();
        for(const auto& row:rows){const auto k=identityKey(row.identity)+':'+row.exchange;if(!confirmed.contains(k)||!reconciled.contains(k))continue;const auto fingerprint=QCryptographicHash::hash((row.rawSignedPrice+'|'+row.rawDate+'|'+row.rawTime+'|'+row.rawLocalTime).toUtf8(),QCryptographicHash::Sha256);
            if(replay.value(k)==fingerprint)continue; // Reconnect replay cannot renew a receipt.
            if(replay.size()>=10000&&!replay.contains(k))continue;replay[k]=fingerprint;values[k]=row;
        }status("키움 공개 실시간 · 원천 UTC 시각 미제공 · 실연결 미검증");notify();
    }
    void dispatch(){
        if(activeJob||!rest||jobs.isEmpty()||activeCountry.isEmpty()||now()<nextDispatch||!kiwoom()||!broker->authenticated())return;
        if(revision&&blocked.contains(revision)&&!authDirty)return;
        const auto candidate=jobs.first();if(!wantedIntent(candidate.intent)){queued.remove(jobKey(candidate.intent));jobs.removeFirst();return;}
        const auto grant=budget->tryAcquire(q,KiwoomBudgetLane::Display,candidate.intent.read==KiwoomDisplayRequest::UsCatalog?KiwoomBudgetRequest::UsCatalogue:KiwoomBudgetRequest::General);
        if(!grant.lease){nextDispatch=add(now(),qMax<qint64>(grant.waitMs,1));return;}const auto epoch=intentEpoch;quint64 sendRevision=0;RequestSpec request;
        const bool authorized=auth([&](QByteArrayView token,quint64 rev){sendRevision=rev;request=KiwoomMarketProtocol::request(candidate.intent);if(request.url.isEmpty())return;QByteArray bearer("Bearer ");bearer.append(token.data(),token.size());request.headers["authorization"]=bearer;bearer.fill('\0');});
        if(!authorized||epoch!=intentEpoch||!sendRevision||request.url.isEmpty()||!wantedIntent(candidate.intent)){
            wipe(request);budget->finish(grant.lease);if(revision&&deadlines.contains(revision)&&now()>=deadlines.value(revision))blocked.insert(revision);authDirty=false;status("기존 키움 인증이 필요하거나 만료되었습니다.");return;
        }
        if(!KiwoomMarketProtocol::isRequest(request)){wipe(request);budget->finish(grant.lease);queued.remove(jobKey(candidate.intent));if(!jobs.isEmpty())jobs.removeFirst();status("키움 공개 요청 계약 확인 실패");return;}
        // Only the public intent is retained. The bearer exists only in this send scope.
        if(!jobs.isEmpty()&&jobKey(jobs.first().intent)==jobKey(candidate.intent))jobs.removeFirst();lease=grant.lease;activeJob=candidate;sentRevision=sendRevision;httpEpoch=intentEpoch;httpAt=now();const auto token=++httpToken;attempted[jobKey(candidate.intent)]=add(now(),60000);rest->send(token,request);wipe(request);
    }
    void result(quint64 token,const HttpResult& response){
        if(!activeJob||token!=httpToken||httpEpoch!=intentEpoch)return;const auto epoch=intentEpoch;const auto rev=sentRevision;const auto job=*activeJob;
        if(!currentReply(rev)||epoch!=intentEpoch){cancelHttp();return;}
        activeJob.reset();queued.remove(jobKey(job.intent));
        if(response.status==401||response.status==403){release();blockCurrent("키움 기존 인증이 거부되었습니다. 직접 다시 인증하세요.");return;}
        if(job.intent.read==KiwoomDisplayRequest::KrDaily){
            const auto daily=KiwoomMarketProtocol::parseDaily(job.intent,response,utc());
            if(daily.authenticationFailed){release();blockCurrent("키움 기존 인증이 거부되었습니다. 직접 다시 인증하세요.");return;}
            if(daily.rateLimited||response.status==429||response.status==418||response.localRetryAfterSeconds>0){const auto delay=retryDelay(response,utc(),response.status==200);release(response.localRetryAfterSeconds>0?0:delay);nextDispatch=qMax(nextDispatch,add(now(),delay));attempted[jobKey(job.intent)]=nextDispatch;enqueue(job,true);status("키움 공유 요청 제한 대기 · 원래 수신 시각 유지");return;}
            release();const WatchlistIdentity identity{"stock","KR",job.intent.symbol};
            if(identity==dailyTarget){
                // A failed refresh replaces the older good series; the chart says why instead of showing old bars.
                dailySeries=daily.available?KiwoomDailySeries{identity,daily.rows,httpAtWall(job),utc(),true,false,{}}:KiwoomDailySeries{identity,{},0,0,false,false,daily.reason};
            }
            if(epoch!=intentEpoch)return;plan();maybeOpen();dispatch();notify();return;
        }
        const bool price=job.intent.read==KiwoomDisplayRequest::KrPrice||job.intent.read==KiwoomDisplayRequest::UsPrice;
        auto page=price?KiwoomMetadataPage{}:KiwoomMarketProtocol::parseMetadata(job.intent,response,utc());auto quote=price?KiwoomMarketProtocol::parseRestPrice(job.intent,response,utc()):KiwoomRestPrice{};
        if(page.authenticationFailed||quote.authenticationFailed){release();blockCurrent("키움 기존 인증이 거부되었습니다. 직접 다시 인증하세요.");return;}
        const bool limited=page.rateLimited||quote.rateLimited||response.status==429||response.status==418||response.localRetryAfterSeconds>0;
        if(limited){const auto delay=retryDelay(response,utc(),response.status==200);release(response.localRetryAfterSeconds>0?0:delay);nextDispatch=qMax(nextDispatch,add(now(),delay));attempted[jobKey(job.intent)]=nextDispatch;enqueue(job,true);status("키움 공유 요청 제한 대기 · 원래 수신 시각 유지");return;}
        release();if(!price){
            if(!page.available){status("키움 공개 종목 정보 확인 실패 · 해당 종목 숨김");return;}Job next=job;next.firstReceipt=next.firstReceipt?qMin(next.firstReceipt,utc()):utc();QSet<QString> seen;for(const auto& i:next.staged)seen.insert(key(i));
            for(const auto& i:page.instruments){if(seen.contains(key(i))||next.staged.size()>=10000){status("키움 종목 정보 중복 또는 범위 초과");return;}seen.insert(key(i));next.staged.append(i);}
            if(page.continued){if(next.pages>=10||next.cursors.contains(page.nextCursor)){status("키움 종목 정보 연속 조회 범위 미확인");return;}++next.pages;next.cursors.insert(page.nextCursor);next.intent.cursor=page.nextCursor;enqueue(next,true,true);dispatch();return;}
            QList<KiwoomVerifiedInstrument> complete;for(auto i:next.staged){i.metadataEvidence.receivedAtMs=next.firstReceipt;i.metadataEvidence.complete=true;complete.append(i);}const auto id="stock:"+job.intent.country+':'+job.intent.symbol;
            if(job.intent.exchange.isEmpty())catalogue[id]=complete;else{auto old=catalogue.value(id);old.erase(std::remove_if(old.begin(),old.end(),[&](const auto& i){return i.exchange==job.intent.exchange;}),old.end());old.append(complete);catalogue[id]=old;}
            if(complete.isEmpty())status("키움 공식 종목 정보를 찾을 수 없습니다.");
            // Complete one metadata->currency proof before the next catalogue
            // intent, so a large list cannot bury the visible first symbol.
            if(complete.size()==1)enqueue(priceJob(complete.first()),true,true);
        }else if(quote.available){
            auto& list=catalogue[identityKey(quote.quote.identity)];bool joined=false;for(auto& i:list)if(i.exchange==quote.quote.exchange&&metadataValid(i,utc(),false)){i.currency=quote.quote.currency;i.currencyEvidence=quote.currencyEvidence;i.currencyApiId=quote.currencyApiId;i.verified=true;joined=true;}
            if(joined){const auto k=identityKey(quote.quote.identity)+':'+quote.quote.exchange;const auto old=values.value(k);if(!old.realtime||old.evidence.receivedAtMs<=httpAtWall(job))values[k]=quote.quote;
                if(job.reconcileWire&&job.reconcileWire==wire&&confirmed.contains(k))reconciled.insert(k);}
        }else status("키움 공개 현재가 대사 실패 · 기존 원천 시각 유지");
        if(epoch!=intentEpoch)return;repartition();plan();maybeOpen();dispatch();notify();
    }
    qint64 httpAtWall(const Job&)const{return utc()-(now()-httpAt);}
    void tick(){
        if(pumping)return;QScopedValueRollback<bool> guard(pumping,true);
        if(!clockValid()){revoke(false);status("시계 변경으로 키움 표시를 중단했습니다.");return;}
        if((wanted.isEmpty()&&dailyTarget.code.isEmpty())||activeCountry.isEmpty())return;
        if(!kiwoom()||!broker->authenticated()){if(wire||closingWire||activeJob||!values.isEmpty()){cancelHttp();invalidateWire();values.clear();catalogue.clear();selected.clear();restOnly.clear();notify();}status("기존 키움 인증이 필요합니다.");return;}
        if(revision&&deadlines.contains(revision)&&now()>=deadlines.value(revision)){blockCurrent("키움 세션 만료 · 기존 연결에서 직접 다시 인증하세요.");return;}
        for(auto it=catalogue.begin();it!=catalogue.end();){auto& list=it.value();list.erase(std::remove_if(list.begin(),list.end(),[&](const auto& i){return !metadataValid(i,utc(),false);}),list.end());if(list.isEmpty())it=catalogue.erase(it);else++it;}
        repartition();if((wire&&wireCountry!=activeCountry)||(wire&&selected.isEmpty()))closeForSelection();
        if(phase==Phase::Closing&&now()-phaseAt>=3000){closingWire=0;phase=Phase::Idle;wireCountry.clear();if(socket)socket->abort();nextOpen=add(now(),1000);}
        else if(wire&&((phase!=Phase::Live&&now()-phaseAt>=15000)||(phase==Phase::Live&&now()-lastHeard>=180000)))failed(wire,0);
        if(activeJob&&now()-httpAt>=15000){const auto retry=*activeJob;cancelHttp();nextDispatch=add(now(),5000);enqueue(retry,true);status("키움 공개 조회 시간 초과 · 대기 후 재시도");}
        plan();maybeOpen();dispatch();
    }
};
KiwoomMarketStream::KiwoomMarketStream(BrokerTradingController* broker,TradingTransport* rest,QObject* parent)
    :KiwoomMarketStream(broker,rest,{}, {},{},parent,{}){}
KiwoomMarketStream::KiwoomMarketStream(BrokerTradingController* broker,TradingTransport* rest,std::unique_ptr<KiwoomMarketSocket> socket,Clock monotonic,Clock utc,QObject* parent,std::shared_ptr<KiwoomRequestBudget> budget)
    :QObject(parent),d_(std::make_unique<Private>()){
    d_->q=this;d_->rest=rest;d_->socket=std::move(socket);d_->budget=budget?std::move(budget):KiwoomRequestBudget::shared();d_->monotonic=monotonic?std::move(monotonic):Clock([]{return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();});d_->utcClock=utc?std::move(utc):Clock(QDateTime::currentMSecsSinceEpoch);
    d_->timer.setInterval(100);connect(&d_->timer,&QTimer::timeout,this,[this]{d_->tick();});
    if(rest){connect(rest,&TradingTransport::finished,this,[this](quint64 token,const HttpResult& r){d_->result(token,r);});connect(rest,&QObject::destroyed,this,[this]{d_->cancelHttp();d_->status("키움 공개 조회 연결이 없습니다.");});}
    if(d_->socket){connect(d_->socket.get(),&KiwoomMarketSocket::connected,this,[this](quint64 g){d_->connected(g);});connect(d_->socket.get(),&KiwoomMarketSocket::received,this,[this](quint64 g,const QByteArray& b){d_->frame(g,b);});connect(d_->socket.get(),&KiwoomMarketSocket::closed,this,[this](quint64 g){d_->disconnected(g);});connect(d_->socket.get(),&KiwoomMarketSocket::failed,this,[this](quint64 g,int s){d_->failed(g,s);});}setBroker(broker);
}
KiwoomMarketStream::~KiwoomMarketStream(){d_->timer.stop();d_->revoke(true);}
void KiwoomMarketStream::setBroker(BrokerTradingController* broker){if(d_->broker==broker)return;d_->rememberSession();d_->revoke(true);d_->restoreSession(broker);disconnect(d_->brokerChanged);disconnect(d_->brokerRevoked);disconnect(d_->brokerDestroyed);if(broker&&!d_->rememberOwner(broker)){d_->broker=nullptr;d_->status("키움 세션 확인 기록 범위 초과 · 시세 연결 중단");return;}d_->broker=broker;d_->authDirty=true;
    if(broker){d_->brokerChanged=connect(broker,&BrokerTradingController::changed,this,[this]{d_->authDirty=true;d_->nextDispatch=0;d_->tick();});d_->brokerRevoked=connect(broker,&BrokerTradingController::intentRevoked,this,[this]{if(d_->revision)d_->blocked.insert(d_->revision);d_->rememberSession();d_->revoke(true);});d_->brokerDestroyed=connect(broker,&QObject::destroyed,this,[this]{d_->broker=nullptr;d_->revoke(true);});}d_->notify();}
BrokerTradingController* KiwoomMarketStream::broker()const{return d_->broker.data();}
void KiwoomMarketStream::setActiveCountry(const QString& country){if((!country.isEmpty()&&country!="KR"&&country!="US")||d_->activeCountry==country)return;d_->activeCountry=country;++d_->intentEpoch;d_->cancelHttp();if(d_->wire)d_->closeForSelection();if(country.isEmpty()){d_->timer.stop();d_->values.clear();d_->selected.clear();d_->restOnly.clear();d_->notify();return;}d_->timer.start();d_->tick();}
void KiwoomMarketStream::setTargets(const QList<KiwoomMarketTarget>& input){
    QList<KiwoomMarketTarget> desired;QSet<QString> seen;if(input.size()<=10000){auto sorted=input;std::stable_sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.priority<b.priority;});for(auto t:sorted){if(!targetValid(t))continue;const auto id=key(t);if(seen.contains(id)){for(auto& kept:desired)if(key(kept)==id)kept.privateOnly=kept.privateOnly&&t.privateOnly;continue;}seen.insert(id);desired.append(t);}}
    if(desired==d_->wanted)return;d_->wanted=desired;++d_->intentEpoch;d_->cancelHttp();if(desired.isEmpty()){d_->invalidateWire();d_->values.clear();d_->selected.clear();d_->restOnly.clear();d_->timer.stop();}else if(!d_->activeCountry.isEmpty())d_->timer.start();d_->tick();d_->notify();
}
bool KiwoomMarketStream::setVerifiedInstruments(const QList<KiwoomVerifiedInstrument>& input,QString* error){
    if(error)error->clear();if(input.size()>10000){if(error)*error="키움 공개 종목 정보 범위 초과";return false;}QMap<QString,QList<KiwoomVerifiedInstrument>> incoming;QSet<QString> seen;
    for(auto i:input){if(!metadataValid(i,d_->utc(),true)||seen.contains(key(i))){if(error)*error="키움 공개 종목 원천·시각·통화 확인 실패";return false;}seen.insert(key(i));i.metadataEvidence.scope="public:kiwoom:"+i.identity.source+':'+i.identity.code+':'+i.exchange;i.currencyEvidence.scope=i.metadataEvidence.scope;i.metadataEvidence.source="키움 공식 공개 종목 정보";i.currencyEvidence.source="키움 공식 통화 확인";incoming[identityKey(i.identity)].append(i);}
    d_->catalogue=incoming;d_->tick();d_->notify();return true;
}
void KiwoomMarketStream::setDailyTarget(const WatchlistIdentity& identity){
    const bool valid=identity.market=="stock"&&identity.source=="KR"&&QRegularExpression("\\A[A-Z0-9]{6}\\z").match(identity.code).hasMatch();
    const auto target=valid?identity:WatchlistIdentity{};if(d_->dailyTarget==target)return;
    d_->dailyTarget=target;if(d_->dailySeries.identity!=target)d_->dailySeries={};
    if(!target.code.isEmpty()&&!d_->activeCountry.isEmpty())d_->timer.start();d_->tick();d_->notify();
}
KiwoomDailySeries KiwoomMarketStream::daily(const WatchlistIdentity& identity)const{
    if(!d_->session()||identity!=d_->dailyTarget)return {};auto result=d_->dailySeries;if(result.identity!=identity)result={identity,{},0,0,false,false,{}};
    KiwoomReadIntent intent;intent.read=KiwoomDisplayRequest::KrDaily;intent.country="KR";intent.symbol=identity.code;
    intent.baseDate=QDateTime::fromMSecsSinceEpoch(d_->utc(),QTimeZone("Asia/Seoul")).date().toString("yyyyMMdd");const auto k=jobKey(intent);
    result.loading=d_->queued.contains(k)||(d_->activeJob&&d_->activeJob->intent.read==KiwoomDisplayRequest::KrDaily);return result;
}
void KiwoomMarketStream::stop(){
    // The window may reconcile an already stopped source from changed(). A
    // second empty stop must not queue another changed event. Any new intent,
    // evidence or in-flight work still takes the full epoch/cancellation path.
    const bool empty=d_->phase==Private::Phase::Idle&&!d_->wire&&!d_->closingWire&&!d_->revision&&!d_->lease&&!d_->activeJob&&
        d_->jobs.isEmpty()&&d_->queued.isEmpty()&&d_->wanted.isEmpty()&&d_->selected.isEmpty()&&d_->restOnly.isEmpty()&&d_->declared.isEmpty()&&
        d_->catalogue.isEmpty()&&d_->values.isEmpty()&&d_->attempted.isEmpty()&&d_->replay.isEmpty()&&d_->confirmed.isEmpty()&&d_->reconciled.isEmpty()&&
        d_->activeCountry.isEmpty()&&d_->wireCountry.isEmpty()&&d_->dailyTarget==WatchlistIdentity{}&&d_->dailySeries.identity==WatchlistIdentity{}&&
        d_->dailySeries.rows.isEmpty()&&!d_->dailySeries.available&&!d_->dailySeries.loading&&!d_->dailySeries.requestStartedAtMs&&!d_->dailySeries.receivedAtMs&&
        d_->dailySeries.reason.isEmpty()&&!d_->timer.isActive();
    if(empty&&d_->message==QStringLiteral("키움 공개 시세 중단"))return;
    d_->revoke(true);d_->status("키움 공개 시세 중단");
}
void KiwoomMarketStream::tick(){d_->tick();}
QList<KiwoomMarketTarget> KiwoomMarketStream::realtimeTargets()const{return d_->selected;}
QList<KiwoomMarketTarget> KiwoomMarketStream::restOnlyTargets()const{return d_->restOnly;}
QList<KiwoomMarketQuote> KiwoomMarketStream::quotes(qint64 at)const{
    QList<KiwoomMarketQuote> result;if(!d_->session())return result;if(!at)at=d_->utc();for(const auto& quote:d_->values){const KiwoomMarketTarget t{quote.identity,quote.exchange,0,false};if(d_->resolve(t,true)&&fresh(quote.evidence,at,QuoteFreshnessMs)){
        const bool currentWs=quote.realtime&&d_->phase==Private::Phase::Live&&d_->confirmed.contains(key(t))&&d_->reconciled.contains(key(t));
        // An old WS observation cannot become a REST observation by changing a
        // flag. Replacement registration needs its own acknowledged baseline.
        if(quote.evidence.url.startsWith("wss:")&&!currentWs)continue;result.append(quote);
    }}return result;
}
QList<KiwoomVerifiedInstrument> KiwoomMarketStream::instruments()const{return instruments(d_->utc());}
QList<KiwoomVerifiedInstrument> KiwoomMarketStream::instruments(qint64 at)const{QList<KiwoomVerifiedInstrument> result;if(!d_->kiwoom())return result;for(const auto& list:d_->catalogue)for(const auto& i:list)if(metadataValid(i,at,true))result.append(i);return result;}
QString KiwoomMarketStream::status()const{return d_->message;}
bool KiwoomMarketStream::authorizationUsable()const{return d_->session();}
QString KiwoomMarketStream::limitations(){return "키움 단일 국가·한 세션 · 최대 200종목 · 미국 Nasdaq TotalView(NBBO 아님) · 원천 UTC 시각 미제공 · 실연결 미검증";}
RequestSpec KiwoomMarketStream::request(KiwoomDisplayRequest read,const QString& country,const QString& symbol,const QString& exchange,const QByteArray& cursor){KiwoomReadIntent i;i.read=read;i.country=country;i.symbol=symbol;i.exchange=exchange;i.cursor=cursor;if(read==KiwoomDisplayRequest::KrCatalog)i.catalogMarket="0";if(read==KiwoomDisplayRequest::UsCatalog&&i.exchange.isEmpty())i.exchange="%";return KiwoomMarketProtocol::request(i);}
bool KiwoomMarketStream::isRequest(const RequestSpec& r){return KiwoomMarketProtocol::isRequest(r);}
QByteArray KiwoomMarketStream::subscription(const QString& country,const QList<KiwoomMarketTarget>& rows,bool remove){return KiwoomMarketProtocol::subscription(country,rows,remove);}
std::optional<KiwoomMarketQuote> KiwoomMarketStream::parseQuote(const QByteArray& bytes,const KiwoomVerifiedInstrument& i,qint64 at){const auto rows=KiwoomMarketProtocol::realtimeQuotes(bytes,i.identity.source,{i},at);return rows.size()==1?std::optional<KiwoomMarketQuote>(rows.first()):std::nullopt;}
}
