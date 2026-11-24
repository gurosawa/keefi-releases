#include "keefi/RankingStudies.h"
#include "keefi/ProductIdentity.h"
#include "keefi/ChartStudies.h"
#include <QDateTime>
#include <QTimeZone>
#include <algorithm>
#include <cmath>
namespace keefi {
QString rankingStudyKey(RankingStudy s){switch(s){case RankingStudy::Compression:return "compression";case RankingStudy::OpenInterest:return "oi";case RankingStudy::SpotFutures:return "spot_futures";case RankingStudy::Pullback:return "pullback";case RankingStudy::Volume:return "volume";case RankingStudy::Drawdown:return "drawdown";default:return "none";}}
QString rankingStudyLabel(RankingStudy s){switch(s){case RankingStudy::Compression:return QStringLiteral("압축");case RankingStudy::OpenInterest:return QStringLiteral("OI 24시간");case RankingStudy::SpotFutures:return QStringLiteral("현물·선물");case RankingStudy::Pullback:return QStringLiteral("눌림 점수");case RankingStudy::Volume:return QStringLiteral("거래량 배수");case RankingStudy::Drawdown:return QStringLiteral("고점 대비");default:return QStringLiteral("상태 숨기기");}}
RankingStudy rankingStudyFromKey(const QString& key){for(auto s:{RankingStudy::Compression,RankingStudy::OpenInterest,RankingStudy::SpotFutures,RankingStudy::Pullback,RankingStudy::Volume,RankingStudy::Drawdown})if(rankingStudyKey(s)==key)return s;return RankingStudy::None;}
RankingStudyValue rankingCandleStudy(RankingStudy study,const ChartStudyObservation& o){
    RankingStudyValue r;
    const auto unavailable=[&](const QString& text,const QString& why=QString{}){r.text=text;r.information=why.isEmpty()?text:why;return r;};
    if(study==RankingStudy::None)return r;
    if(study==RankingStudy::OpenInterest||study==RankingStudy::SpotFutures)return unavailable(QStringLiteral("코인 선물 전용"));
    if(o.delayed)return unavailable(QStringLiteral("지연"));
    if(!o.series.available)return unavailable(o.status.isEmpty()?QStringLiteral("받는 중"):o.status);
    const auto& rows=o.series.candles;
    if(o.series.intervalMs!=3600000||rows.isEmpty()||rows.size()>200)return unavailable(QStringLiteral("기록 부족"));
    if(o.stock&&(o.timeline.rows.size()!=rows.size()||!o.timeline.adjustedComparable))
        return unavailable(QStringLiteral("기준 확인 필요"),o.timeline.comparisonReason);
    int begin=0;
    for(int i=0;i<rows.size();++i){
        const auto& c=rows[i];bool ok=false;double values[5];int n=0;
        for(const auto& text:{c.open,c.high,c.low,c.close,c.volume}){double v=text.toDouble(&ok);if(!ok||!std::isfinite(v)||(n<4?v<=0:v<0))return unavailable(QStringLiteral("자료 확인 필요"));values[n++]=v;}
        if(c.startsAtMs<=0||(i&&c.startsAtMs<=rows[i-1].startsAtMs)||values[2]>std::min(values[0],values[3])||values[1]<std::max(values[0],values[3]))return unavailable(QStringLiteral("자료 확인 필요"));
        if(o.stock){const auto& t=o.timeline.rows[i];
            if(t.startsAtMs!=c.startsAtMs||t.completesAtMs<=t.startsAtMs||(t.connectsPrevious&&t.missingSlotsBefore&&*t.missingSlotsBefore!=0))return unavailable(QStringLiteral("기준 확인 필요"));
            if(!t.eligible)begin=i+1;else if(i&&(!t.connectsPrevious||!o.timeline.rows[i-1].eligible))begin=i;
        }else if(i&&c.startsAtMs-rows[i-1].startsAtMs!=3600000)begin=i;
    }
    const int count=rows.size()-begin;
    if(o.stock&&!o.timeline.rows.last().eligible)return unavailable(QStringLiteral("기준 확인 필요"),o.timeline.rows.last().reason.isEmpty()?QStringLiteral("마지막 완료 봉의 판정 근거를 확인할 수 없어요."):o.timeline.rows.last().reason);
    if(count<(study==RankingStudy::Volume?21:20))return unavailable(QStringLiteral("기록 부족"),QStringLiteral("누락 없이 이어진 완료 1시간봉이 필요해요."));
    r.throughMs=o.stock?o.timeline.rows.last().completesAtMs:rows.last().startsAtMs+3600000;
    r.information=QStringLiteral("완료 1시간봉 · 봉 완료 후 갱신 · 현재 봉 제외\n")+o.series.sourceUrl+"\n"+QDateTime::fromMSecsSinceEpoch(r.throughMs,QTimeZone("Asia/Seoul")).toString("MM/dd HH:mm KST");
    if(study==RankingStudy::Compression){const auto value=o.stock?candleCompression(rows,3600000,o.timeline):candleCompression(rows,3600000);
        if(!value.available)return unavailable(QStringLiteral("기록 부족"),value.reason);r.text=value.compressed?QStringLiteral("압축 중"):QStringLiteral("압축 아님");r.information+=QStringLiteral("\nBB 20·2σ가 KC EMA20·ATR20×1.5 안에 있는지 표시합니다.");
    }else if(study==RankingStudy::Pullback){const auto value=o.stock?keefiChartStudies(rows,3600000,o.timeline):keefiChartStudies(rows,3600000);
        if(!value.pullback.available)return unavailable(QStringLiteral("기록 부족"),value.pullback.reason);r.text=QStringLiteral("%1/10").arg(value.pullback.score);r.information+=QStringLiteral("\n%1 눌림존의 겹침 점수입니다. 상태 설명이며 매매 신호가 아닙니다.").arg(productName());
    }else if(study==RankingStudy::Volume){QList<double> history;for(int i=rows.size()-21;i<rows.size()-1;++i)history.append(rows[i].volume.toDouble());std::sort(history.begin(),history.end());const double median=(history[9]+history[10])/2;
        if(median<=0)return unavailable(QStringLiteral("계산할 수 없어요"),QStringLiteral("직전 20개 완료 봉의 거래량 중앙값이 0입니다."));
        const double multiple=rows.last().volume.toDouble()/median;if(!std::isfinite(multiple))return unavailable(QStringLiteral("계산할 수 없어요"));
        r.text=QStringLiteral("%1배").arg(multiple,0,'f',2);r.information+=QStringLiteral("\n마지막 완료 봉 거래량 ÷ 그 직전 20봉 거래량 중앙값. 종목 내 비교만 합니다.");
    }else if(study==RankingStudy::Drawdown){double high=0;for(int i=rows.size()-20;i<rows.size();++i)high=std::max(high,rows[i].high.toDouble());const double pct=std::max(0.0,100*(1-rows.last().close.toDouble()/high));
        r.text=(pct<.005?QString{}:QStringLiteral("▼"))+QString::number(pct,'f',2)+"%";r.information+=QStringLiteral("\n최근 20개 완료 봉의 최고가 대비 마지막 종가 하락폭. 상장 후 최고가 대비가 아닙니다.");
    }
    r.available=!r.text.isEmpty();return r;
}
}
