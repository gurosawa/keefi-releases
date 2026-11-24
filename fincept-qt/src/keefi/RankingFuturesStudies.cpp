#include "keefi/RankingFuturesFeed.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <limits>
namespace keefi {
namespace {
constexpr qint64 Hour=3600000;
RankingStudyValue missing(QString text=QStringLiteral("자료 부족")){
    return {false,std::move(text),QStringLiteral("같은 종목과 완료 시각의 공개 원문을 확인할 수 없어요"),0};
}
bool success(const HttpResult& r){return r.status==200&&!r.timedOut&&!r.networkError&&r.localRetryAfterSeconds<=0;}
bool baseValid(const QString& base){static const QRegularExpression pattern("\\A[A-Z0-9]{1,20}\\z");return pattern.match(base).hasMatch();}
bool number(const QString& value){static const QRegularExpression pattern("\\A(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?\\z");return value.size()<=512&&pattern.match(value).hasMatch();}
qint64 timestamp(const QJsonValue& value){bool ok=false;const auto text=value.toString();const auto n=text.toLongLong(&ok);return ok&&number(text)&&!text.contains('.')&&n>0?n:0;}
QString direction(int sign){return sign>0?QStringLiteral("▲"):sign<0?QStringLiteral("▼"):QStringLiteral("균형");}
struct QuoteFlow {QString quote,buy;int direction=0;double fraction=0;};
std::optional<QuoteFlow> quoteFlow(const HttpResult& response,qint64 asOf,int maximum){
    if(!success(response))return {};
    const auto document=parseExactJson(response.body);
    if(!document.isArray()||document.array().isEmpty()||document.array().size()>maximum)return {};
    const auto through=asOf/Hour*Hour;QMap<qint64,QJsonArray> completed;QSet<qint64> seen;
    for(const auto& value:document.array()){
        if(!value.isArray())return {};
        const auto row=value.toArray();if(row.size()!=12)return {};
        const auto from=timestamp(row[0]),end=timestamp(row[6]);
        if(from<=0||from%Hour||from>through||from>std::numeric_limits<qint64>::max()-Hour||end!=from+Hour-1||seen.contains(from))return {};
        seen.insert(from);
        for(const int index:{1,2,3,4,5,7,9,10,11})if(!number(row[index].toString()))return {};
        const auto open=row[1].toString(),high=row[2].toString(),low=row[3].toString(),close=row[4].toString();
        if(!decimal::positive(open)||!decimal::positive(high)||!decimal::positive(low)||!decimal::positive(close)
            ||decimal::compare(low,open)>0||decimal::compare(low,close)>0||decimal::compare(high,open)<0||decimal::compare(high,close)<0
            ||decimal::compare(row[9].toString(),row[5].toString())>0||decimal::compare(row[10].toString(),row[7].toString())>0)return {};
        const auto count=row[8].toString();if(!number(count)||count.contains('.'))return {};
        if(from+Hour<=through)completed.insert(from,row); // Forming row never enters the comparison.
    }
    qint64 previous=0;for(auto it=completed.cbegin();it!=completed.cend();++it){if(previous&&it.key()!=previous+Hour)return {};previous=it.key();}
    if(!completed.contains(through-Hour))return {};
    const auto row=completed.value(through-Hour);QuoteFlow flow;flow.quote=row[7].toString();flow.buy=row[10].toString();
    if(!decimal::positive(flow.quote))return {};
    const auto twice=decimal::multiply(flow.buy,"2");flow.direction=decimal::compare(twice,flow.quote);
    const auto total=flow.quote.toDouble(),buy=flow.buy.toDouble();
    if(!std::isfinite(total)||!std::isfinite(buy)||total<=0)return {};
    flow.fraction=2*(buy/total)-1;if(!std::isfinite(flow.fraction))return {};
    return flow;
}
}
RankingStudyValue RankingFuturesFeed::parseOpenInterest24h(const QString& base,const HttpResult& response,qint64 asOf){
    if(!baseValid(base)||asOf<=25*Hour||!success(response))return missing();
    const auto document=parseExactJson(response.body);
    if(!document.isArray()||document.array().size()<25||document.array().size()>200)return missing();
    const auto end=asOf/Hour*Hour;QMap<qint64,QString> quantities;
    for(const auto& value:document.array()){
        if(!value.isObject())return missing();
        const auto row=value.toObject();const auto at=timestamp(row["timestamp"]);const auto quantity=row["sumOpenInterest"].toString();
        if(row["symbol"].toString()!=base+"USDT"||at<=0||at%Hour||at>end||quantities.contains(at)||!number(quantity))return missing();
        quantities.insert(at,quantity);
    }
    for(qint64 at=end-24*Hour;at<=end;at+=Hour)if(!quantities.contains(at))return missing();
    const auto first=quantities.value(end-24*Hour),last=quantities.value(end);
    if(!decimal::positive(first))return missing();
    const auto initial=first.toDouble(),current=last.toDouble();
    if(!std::isfinite(initial)||!std::isfinite(current)||initial<=0)return missing();
    const auto percent=100*(current/initial-1);if(!std::isfinite(percent))return missing();
    const auto sign=decimal::compare(last,first);
    return {true,QStringLiteral("%1 %2%").arg(direction(sign),QString::number(std::abs(percent),'f',2)),
        QStringLiteral("미결제약정 수량(%1)의 정확히 24시간 전 대비 변화예요. 가격이나 시총가치는 섞지 않아요. 계약 수량 변화이며 방향 예측이 아니에요.").arg(base),end};
}
RankingStudyValue RankingFuturesFeed::parseSpotFutures(const QString& base,const HttpResult& spot,const HttpResult& futures,qint64 asOf){
    if(!baseValid(base)||asOf<=Hour)return missing();
    const auto a=quoteFlow(spot,asOf,200),b=quoteFlow(futures,asOf,3);if(!a||!b)return missing();
    return {true,QStringLiteral("현물 %1 · 선물 %2").arg(direction(a->direction),direction(b->direction)),
        QStringLiteral("%1/USDT 현물과 동일 자산 USDT 무기한 계약의 같은 완료 1시간을 비교해요. USDT 거래대금과 시장가 매수 거래대금의 정규화 비율 (2 × 매수 거래대금 − 전체 거래대금) / 전체 거래대금이에요. 현물 %2 · 선물 %3. 누적 체결 흐름이나 방향 예측이 아니에요.")
            .arg(base,QString::number(a->fraction,'f',4),QString::number(b->fraction,'f',4)),asOf/Hour*Hour};
}
}
