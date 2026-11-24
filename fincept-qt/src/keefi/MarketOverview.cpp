#include "keefi/MarketOverview.h"
#include "keefi/TradingTransport.h"
#include "keefi/XmlInputGuard.h"
#include <QDateTime>
#include <QPointer>
#include <QTimer>
#include <QTimeZone>
#include <QUrlQuery>
#include <QXmlStreamReader>

namespace keefi {
namespace {
const QString Atom="http://www.w3.org/2005/Atom";
const QString Data="http://schemas.microsoft.com/ado/2007/08/dataservices";
const QString Metadata="http://schemas.microsoft.com/ado/2007/08/dataservices/metadata";
QDate usDate(qint64 time){return QDateTime::fromMSecsSinceEpoch(time,QTimeZone("America/New_York")).date();}
QDate monthStart(QDate date){return QDate(date.year(),date.month(),1);}
DailyTreasuryYield unavailable(DailyTreasuryYield data,const QString& reason){data.available=false;data.percent.clear();data.reason=reason;return data;}
}
struct MarketOverview::Private {
    QPointer<TradingTransport> transport;QTimer timer,retry;DailyTreasuryYield data;
    Clock clock;
    bool enabled=false,fallback=false;quint64 token=0,nextToken=1;QDate month;qint64 lastAttempt=0;
    void send(){token=nextToken++;transport->send(token,MarketOverview::request(month));}
    qint64 now()const{return clock?clock():QDateTime::currentMSecsSinceEpoch();}
};
MarketOverview::MarketOverview(TradingTransport* transport,QObject* parent):MarketOverview(transport,Clock{},parent){}
MarketOverview::MarketOverview(TradingTransport* transport,Clock clock,QObject* parent):QObject(parent),d_(std::make_unique<Private>()){
    d_->transport=transport;d_->clock=std::move(clock);d_->data.reason=QStringLiteral("공개 자료 조회 대기");d_->timer.setInterval(30*60000);
    connect(&d_->timer,&QTimer::timeout,this,&MarketOverview::refresh);d_->retry.setSingleShot(true);
    connect(&d_->retry,&QTimer::timeout,this,&MarketOverview::refresh);
    if(transport){
        connect(transport,&TradingTransport::finished,this,[this](quint64 token,const HttpResult& result){
            if(!token||token!=d_->token||!d_->enabled)return;
            d_->token=0;d_->data=parse(result,d_->month,d_->now());
            if(d_->data.noObservations&&!d_->fallback){
                d_->fallback=true;d_->month=d_->month.addMonths(-1);d_->send();
            }
            emit changed();
        });
        connect(transport,&QObject::destroyed,this,[this]{d_->transport=nullptr;d_->token=0;d_->timer.stop();d_->data=unavailable(d_->data,QStringLiteral("공개 자료 연결 없음"));emit changed();});
    }
}
MarketOverview::~MarketOverview(){disconnect();setEnabled(false);}
void MarketOverview::setEnabled(bool enabled){
    if(d_->enabled==enabled)return;d_->enabled=enabled;
    if(enabled){d_->timer.start();refresh();}
    else{d_->timer.stop();d_->retry.stop();const bool pending=d_->token!=0;d_->token=0;
        if(pending){d_->data=unavailable(d_->data,QStringLiteral("공개 자료 조회 대기"));if(d_->transport)d_->transport->abort();}}
}
void MarketOverview::refresh(){
    if(!d_->enabled||d_->token)return;
    if(!d_->transport){d_->data=unavailable(d_->data,QStringLiteral("공개 자료 연결 없음"));emit changed();return;}
    const auto now=d_->now();
    // A rapid tab hide/show does not repeatedly request the same public feed.
    if(d_->lastAttempt>0&&now-d_->lastAttempt>=0&&now-d_->lastAttempt<60000){d_->retry.start(int(60000-(now-d_->lastAttempt)));return;}
    d_->lastAttempt=now;d_->month=monthStart(usDate(now));d_->fallback=false;
    // A refresh request is not a new observation. Keep the previous value and
    // receipt time; treasury10Year() still applies the original freshness limit.
    if(!d_->data.available)d_->data=unavailable({},QStringLiteral("미국 재무부 자료를 불러오는 중입니다."));
    d_->send();emit changed();
}
bool MarketOverview::busy()const{return d_->token!=0;}
DailyTreasuryYield MarketOverview::treasury10Year(qint64 asOfMs)const{
    const auto now=asOfMs>0?asOfMs:d_->now();auto data=d_->data;
    if(!data.available)return data;
    if(data.receivedAtMs>now||data.observationDate>usDate(now))return unavailable(data,QStringLiteral("관측 시각을 확인할 수 없습니다."));
    if(now-data.receivedAtMs>SourceRecheckMs)return unavailable(data,QStringLiteral("공식 자료를 다시 확인해야 합니다."));
    if(data.observationDate.daysTo(usDate(now))>MaximumObservationAgeDays)return unavailable(data,QStringLiteral("관측일이 7일 넘은 자료입니다."));
    return data;
}
MarketTimeMode MarketOverview::effectiveMode(MarketTimeMode mode,qint64 asOfMs){
    if(mode!=MarketTimeMode::Automatic)return mode;
    const auto time=QDateTime::fromMSecsSinceEpoch(asOfMs,QTimeZone("Asia/Seoul")).time();
    if(time>=QTime(7,0)&&time<QTime(9,0))return MarketTimeMode::Morning;
    if(time>=QTime(9,0)&&time<QTime(21,0))return MarketTimeMode::Korea;
    return MarketTimeMode::Night;
}
QString MarketOverview::modeLabel(MarketTimeMode mode){
    switch(mode){case MarketTimeMode::Automatic:return QStringLiteral("자동");case MarketTimeMode::Morning:return QStringLiteral("아침");case MarketTimeMode::Korea:return QStringLiteral("국장");case MarketTimeMode::Night:return QStringLiteral("밤");}
    return {};
}
QString MarketOverview::modeContext(MarketTimeMode mode,qint64 asOfMs){
    const auto effective=effectiveMode(mode,asOfMs);const auto time=QDateTime::fromMSecsSinceEpoch(asOfMs,QTimeZone("Asia/Seoul")).time();
    QString context;
    if(effective==MarketTimeMode::Morning)context=QStringLiteral("간밤 미국 시장과 국내 시장 준비");
    else if(effective==MarketTimeMode::Korea)context=(time>=QTime(15,30)&&time<QTime(21,0))?QStringLiteral("국장 마감 후 맥락 · 밤 시장 준비"):QStringLiteral("국내 시장과 원화 흐름");
    else context=QStringLiteral("미국 시장과 야간 흐름");
    return modeLabel(effective)+QStringLiteral(" · ")+context+QStringLiteral(" · KST 시간대 보기");
}
RequestSpec MarketOverview::request(const QDate& month){
    RequestSpec r;r.operation=Operation::Markets;
    if(!month.isValid()||month.year()<1990||month.year()>9999)return r;
    r.url=QUrl("https://home.treasury.gov/resource-center/data-chart-center/interest-rates/pages/xml");
    QUrlQuery query;query.addQueryItem("data","daily_treasury_yield_curve");query.addQueryItem("field_tdr_date_value_month",month.toString("yyyyMM"));r.url.setQuery(query);
    r.headers.insert("Accept","application/xml");return r;
}
bool MarketOverview::isRequest(const RequestSpec& r){
    if(r.operation!=Operation::Markets||r.method!="GET"||r.authenticated||!r.body.isEmpty())return false;
    const auto value=QUrlQuery(r.url).queryItemValue("field_tdr_date_value_month");
    const auto month=QDate::fromString(value+"01","yyyyMMdd");const auto expected=request(month);
    return expected.url.isValid()&&!expected.url.isEmpty()&&r.url==expected.url&&r.headers==expected.headers;
}
DailyTreasuryYield MarketOverview::parse(const HttpResult& response,const QDate& requestedMonth,qint64 receivedAtMs){
    DailyTreasuryYield result;result.source=QStringLiteral("미국 재무부");result.sourceUrl=request(requestedMonth).url.toString();result.receivedAtMs=receivedAtMs;
    const auto fail=[&](const QString& reason){return unavailable(result,reason);};
    if(response.status!=200||response.networkError||response.timedOut)return fail(response.status==429?QStringLiteral("원천 요청 제한"):QStringLiteral("미국 재무부 자료 연결 실패"));
    if(receivedAtMs<=0||!requestedMonth.isValid()||response.body.isEmpty()||response.body.size()>1024*1024)return fail(QStringLiteral("공식 자료의 범위를 확인할 수 없습니다."));
    if(!xmlAttributeBudgetAllows(response.body))return fail(QStringLiteral("공식 자료의 형식을 확인할 수 없습니다."));
    QXmlStreamReader xml(response.body);int depth=0,properties=0,entries=0;bool root=false,inEntry=false,hasDate=false,hasRate=false,rateNull=false;
    QString dateText,rateText;QMap<QDate,QString> rows;QDate latest;
    while(!xml.atEnd()){
        const auto token=xml.readNext();
        if(token==QXmlStreamReader::DTD||token==QXmlStreamReader::EntityReference)return fail(QStringLiteral("공식 자료의 형식을 확인할 수 없습니다."));
        if(token==QXmlStreamReader::StartElement){
            ++depth;const auto name=xml.name();const auto ns=xml.namespaceUri();
            if(depth==1){if(name!="feed"||ns!=Atom)return fail(QStringLiteral("공식 자료의 형식을 확인할 수 없습니다."));root=true;}
            else if(depth==2&&name=="entry"&&ns==Atom){
                if(++entries>64)return fail(QStringLiteral("공식 월별 자료의 범위를 확인할 수 없습니다."));
                inEntry=true;hasDate=hasRate=rateNull=false;dateText.clear();rateText.clear();
            }else if(inEntry&&depth==4&&name=="properties"&&ns==Metadata)properties=depth;
            else if(properties&&depth==properties+1&&ns==Data&&(name=="NEW_DATE"||name=="BC_10YEAR")){
                const bool date=name=="NEW_DATE";
                if(date?hasDate:hasRate)return fail(QStringLiteral("공식 자료에 중복된 값이 있습니다."));
                if(date)hasDate=true;else{hasRate=true;rateNull=xml.attributes().value(Metadata,"null")=="true";}
                const auto value=xml.readElementText(QXmlStreamReader::ErrorOnUnexpectedElement).trimmed();--depth;
                if(date)dateText=value;else rateText=value;
            }
        }else if(token==QXmlStreamReader::EndElement){
            if(inEntry&&depth==2&&xml.name()=="entry"){
                inEntry=false;const auto date=QDate::fromString(dateText.left(10),"yyyy-MM-dd");
                if(!hasDate||!date.isValid()||!QDateTime::fromString(dateText,Qt::ISODate).isValid()||date.year()!=requestedMonth.year()||date.month()!=requestedMonth.month())
                    return fail(QStringLiteral("공식 관측일을 확인할 수 없습니다."));
                if(date<=usDate(receivedAtMs)){
                    const auto rate=hasRate&&!rateNull&&rateText.size()<=16&&decimal::valid(rateText)?decimal::normalize(rateText):QString{};
                    if(rows.contains(date)&&rows.value(date)!=rate)return fail(QStringLiteral("같은 관측일의 공식 값이 서로 다릅니다."));
                    rows.insert(date,rate);if(!latest.isValid()||date>latest)latest=date;
                }
            }
            if(properties==depth)properties=0;--depth;
        }
    }
    if(xml.hasError()||!root||depth!=0)return fail(QStringLiteral("공식 자료의 형식을 확인할 수 없습니다."));
    if(!latest.isValid()){result.noObservations=true;return fail(QStringLiteral("이 달에 확인된 공시 자료가 없습니다."));}
    result.observationDate=latest;
    if(rows.value(latest).isEmpty())return fail(QStringLiteral("최근 관측일의 10년물 값이 제공되지 않았습니다."));
    if(latest.daysTo(usDate(receivedAtMs))>MaximumObservationAgeDays)return fail(QStringLiteral("관측일이 7일 넘은 자료입니다."));
    result.percent=rows.value(latest);result.available=true;return result;
}
} // namespace keefi
