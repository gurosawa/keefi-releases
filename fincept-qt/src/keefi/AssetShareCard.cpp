#include "keefi/AssetShareCard.h"
#include "keefi/ProductIdentity.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/TradingTypes.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QImageWriter>
#include <QLocale>
#include <QMap>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimeZone>
#include <algorithm>
#include <cmath>

namespace keefi {
namespace {
QString sourceName(ChartShareSource source) {
    switch(source) {
    case ChartShareSource::Upbit: return QStringLiteral("업비트");
    case ChartShareSource::BinanceSpot: return QStringLiteral("Binance Spot");
    case ChartShareSource::Toss: return QStringLiteral("토스");
    }
    return {};
}
QString stamp(qint64 at, const QByteArray& zone) {
    return QDateTime::fromMSecsSinceEpoch(at,QTimeZone(zone)).toString(QStringLiteral("yyyy.MM.dd HH:mm"));
}
QString custodyName(const QString& provider) {
    static const QMap<QString,QString> names{{"upbit","업비트"},{"binance","Binance"},{"bithumb","빗썸"},
        {"phantom","Phantom 지갑"},{"okx","OKX"},{"toss","토스"},{"kiwoom","키움"},{"kis","한국투자증권"},{"ls","LS증권"}};
    return names.value(provider);
}
void text(QPainter& painter, const QRectF& rect, const QString& value, FontRole role,
          const QColor& color, Qt::Alignment alignment=Qt::AlignLeft|Qt::AlignVCenter, qreal enlargement=1) {
    const auto font=KeefiFonts::font(role);
    const auto width=QFontMetricsF(font).horizontalAdvance(value);
    const qreal scale=width>0?qMin(enlargement,rect.width()/width):enlargement;
    painter.save();painter.translate(rect.topLeft());painter.scale(scale,scale);
    painter.setFont(font);painter.setPen(color);
    painter.drawText(QRectF(0,0,rect.width()/scale,rect.height()/scale),int(alignment)|Qt::TextSingleLine,value);
    painter.restore();
}
}

qint64 AssetShareCard::intervalMs(AssetSharePeriod period) {
    switch(period){case AssetSharePeriod::Day:return 900000;case AssetSharePeriod::Week:return 3600000;
    case AssetSharePeriod::Month:return 14400000;case AssetSharePeriod::HalfYear:return 86400000;}return 0;
}
qint64 AssetShareCard::durationMs(AssetSharePeriod period) {
    switch(period){case AssetSharePeriod::Day:return 86400000;case AssetSharePeriod::Week:return 7LL*86400000;
    case AssetSharePeriod::Month:return 30LL*86400000;case AssetSharePeriod::HalfYear:return 183LL*86400000;}return 0;
}
QString AssetShareCard::periodLabel(AssetSharePeriod period) {
    switch(period){case AssetSharePeriod::Day:return QStringLiteral("1일");case AssetSharePeriod::Week:return QStringLiteral("7일");
    case AssetSharePeriod::Month:return QStringLiteral("30일");case AssetSharePeriod::HalfYear:return QStringLiteral("6개월(183일)");}return {};
}
AssetShareRenderResult AssetShareCard::render(const AssetShareSnapshot& value, const AssetShareOptions& options) {
    AssetShareRenderResult result;
    result.reason=QStringLiteral("완료된 공개 가격과 비교 구간을 확인할 수 없습니다.");
    if(options.outputSize.width()<640||options.outputSize.height()<720||
       options.outputSize.width()>4096||options.outputSize.height()>4096)return result;
    const auto& market=value.market;
    static const QRegularExpression assetName(QStringLiteral("\\A[\\p{L}\\p{N} .·()_-]{1,80}\\z"));
    static const QRegularExpression symbol(QStringLiteral("\\A[A-Z0-9._-]{1,24}\\z"));
    if(!assetName.match(value.assetName).hasMatch()||!symbol.match(value.assetSymbol).hasMatch()||
       value.requestedFromMs<=0||value.requestedToMs<=value.requestedFromMs||
       value.requestedToMs-value.requestedFromMs!=durationMs(value.period)||market.intervalMs!=intervalMs(value.period))return result;
    // Reuse the existing canonical source/decimal/timeline/study validation.
    // No arbitrary source labels, profile data or widget captures enter this card.
    ChartShareOptions validation;validation.outputSize={640,480};
    if((market.availability!=ChartShareAvailability::Ready&&market.availability!=ChartShareAvailability::Stale)||
       !ChartShareCard::render(market,validation).available)return result;
    const bool personalRequested=options.includeQuantity||options.includeValuation||options.includeCustody||options.includeTodayObservation;
    QStringList personalRows;
    if(personalRequested) {
        result.reason=QStringLiteral("선택한 개인값의 값과 확인 시각을 확인할 수 없습니다.");
        static const QRegularExpression number(QStringLiteral("\\A(?:0|[1-9][0-9]{0,79})(?:\\.[0-9]{1,80})?\\z"));
        static const QRegularExpression currency(QStringLiteral("\\A[A-Z0-9]{2,12}\\z"));
        if(!value.personal||value.personal->observedAtMs<=0||value.personal->observedAtMs>market.asOfMs) {
            result.reason=QStringLiteral("보유 숫자의 값과 확인 시각을 확인할 수 없습니다.");return result;
        }
        const auto& personal=*value.personal;
        const auto observed=QStringLiteral(" · 관측 ")+stamp(personal.observedAtMs,"Asia/Seoul")+" KST";
        if(options.includeQuantity){if(!number.match(personal.quantity).hasMatch())return result;
            personalRows.append(QStringLiteral("현재 수량  ")+groupedAmount(personal.quantity)+" "+value.assetSymbol+observed);}
        if(options.includeValuation){if(!number.match(personal.currentValuation).hasMatch()||!currency.match(personal.currency).hasMatch())return result;
            personalRows.append(QStringLiteral("현재 평가액  ")+groupedAmount(personal.currentValuation)+" "+personal.currency+
                (personal.valuationComplete?QStringLiteral(" · %1 평가 · 완전").arg(productName()):QStringLiteral(" · %1 평가 · 부분").arg(productName()))+observed);}
        if(options.includeCustody){QStringList names;if(personal.custodyProviders.isEmpty()||personal.custodyProviders.size()>9)return result;
            for(const auto& provider:personal.custodyProviders){const auto name=custodyName(provider);if(name.isEmpty())return result;if(!names.contains(name))names.append(name);}
            personalRows.append(QStringLiteral("보관처  ")+names.join(" · "));}
        if(options.includeTodayObservation){
            if(!personal.today)return result;const auto& today=*personal.today;auto magnitude=today.difference;QString sign;
            if(magnitude.startsWith('+')||magnitude.startsWith('-')){sign=magnitude.left(1);magnitude.remove(0,1);}
            const auto asOf=QDateTime::fromMSecsSinceEpoch(market.asOfMs,QTimeZone("Asia/Seoul"));
            if(!number.match(magnitude).hasMatch()||!currency.match(today.currency).hasMatch()||today.currency!=personal.currency||
               today.fromMs<asOf.date().startOfDay(asOf.timeZone()).toMSecsSinceEpoch()||today.toMs<=today.fromMs||
               today.toMs>personal.observedAtMs)return result;
            personalRows.append(QStringLiteral("관측 평가액 증감 · 입출금 포함 가능 · 오늘 0시 이후"));
            personalRows.append(sign+groupedAmount(magnitude)+" "+today.currency+QStringLiteral(" · 비교 ")+stamp(today.fromMs,"Asia/Seoul")+
                " → "+stamp(today.toMs,"Asia/Seoul")+" KST");
        }
        result.personalValuesIncluded=true;
    }
    QList<int> visible;
    for(int i=0;i<market.closed.size();++i)
        if(market.closed[i].startsAtMs>=market.viewport.fromMs&&market.closed[i].startsAtMs<=market.viewport.toMs)visible.append(i);
    if(visible.size()<2)return result;
    for(int i:visible)if(!decimal::positive(market.closed[i].close))return result;
    const auto completed=[&](int i){return market.stockTimeline?market.stockTimeline->rows[i].completesAtMs:market.closed[i].startsAtMs+market.intervalMs;};
    result.startPrice=market.closed[visible.first()].close;result.currentPrice=market.closed[visible.last()].close;
    const int direction=decimal::compare(result.currentPrice,result.startPrice);
    const auto magnitude=direction<0?decimal::subtract(result.startPrice,result.currentPrice):decimal::subtract(result.currentPrice,result.startPrice);
    if(magnitude.isEmpty())return result;
    const double percentage=magnitude.toDouble()/result.startPrice.toDouble()*100;
    if(!std::isfinite(percentage))return result;
    const auto sign=direction>0?QStringLiteral("+"):direction<0?QStringLiteral("-"):QString{};
    result.changeAmount=sign+magnitude;
    result.changePercent=sign+QLocale::c().toString(percentage,'f',2)+"%";
    result.fromMs=completed(visible.first());result.toMs=completed(visible.last());
    if(result.toMs<=result.fromMs)return result;
    if(result.fromMs!=value.requestedFromMs){result.reason=QStringLiteral("이 기간의 시작 봉이 없어요.");return result;}
    if(result.toMs!=value.requestedToMs){result.reason=QStringLiteral("이 기간의 끝 봉이 없어요.");return result;}
    const auto title=value.assetName==value.assetSymbol?value.assetSymbol:value.assetName+" "+value.assetSymbol;
    const auto period=stamp(result.fromMs,"Asia/Seoul")+" → "+stamp(result.toMs,"Asia/Seoul")+" KST (UTC+9) · "+periodLabel(value.period);
    const auto asOf=QStringLiteral("기준 ")+stamp(market.asOfMs,"Asia/Seoul")+" KST (UTC+9)";
    const auto source=sourceName(market.source.provider)+" "+market.identity.code+QStringLiteral(" · 완료봉 종가")+
        (market.availability==ChartShareAvailability::Stale?QStringLiteral(" · 지연 자료"):QString{});
    const auto change=sign+groupedAmount(magnitude)+" "+market.currency;
    const auto start=groupedAmount(result.startPrice)+" "+market.currency,current=groupedAmount(result.currentPrice)+" "+market.currency;
    const auto privacy=personalRequested?QStringLiteral("선택한 개인값 포함 · 원가·수익률 아님 · 로컬 저장"):QStringLiteral("개인값 제외 · 원가·수익률 아님 · 로컬 저장");
    result.heroText=result.changePercent;
    result.captions={title,asOf,QStringLiteral("시장 가격 변화"),result.heroText,change,
                     QStringLiteral("시작 ")+start,QStringLiteral("현재 ")+current,period,source,
                     privacy,productName()};
    const auto& colors=KeefiTheme::colorsFor(true);
    QImage image(options.outputSize,QImage::Format_ARGB32_Premultiplied);
    if(image.isNull())return result;
    image.setDevicePixelRatio(1);image.setDotsPerMeterX(3780);image.setDotsPerMeterY(3780);image.fill(colors.studyPurple);
    QPainter painter(&image);if(!painter.isActive())return result;
    painter.setRenderHint(QPainter::Antialiasing);painter.scale(image.width()/1000.0,image.height()/1200.0);
    painter.setPen(Qt::NoPen);painter.setBrush(colors.bg);painter.drawRoundedRect(QRectF(24,24,952,1152),28,28);
    text(painter,{68,64,864,54},title,FontRole::Lead28,colors.text,Qt::AlignLeft|Qt::AlignVCenter,1.4);
    text(painter,{68,125,864,26},asOf,FontRole::Secondary14,colors.text2);
    const auto movement=direction>0?colors.upText:direction<0?colors.downText:colors.text;
    const QRectF area(68,210,864,290);
    QString low=market.closed[visible.first()].close,high=low;
    for(int i:visible){const auto& close=market.closed[i].close;
        if(decimal::compare(close,low)<0)low=close;if(decimal::compare(close,high)>0)high=close;}
    // Subtract exact prices before converting to pixels: large absolute prices
    // can differ by less than one double unit while their public change is real.
    const double span=decimal::subtract(high,low).toDouble();
    const auto point=[&](int i){return QPointF(area.left()+double(completed(i)-result.fromMs)/double(result.toMs-result.fromMs)*area.width(),
        span>0?area.bottom()-decimal::subtract(market.closed[i].close,low).toDouble()/span*area.height():area.center().y());};
    QPainterPath path;bool gap=false;int previous=-1;
    for(int i:visible){const auto p=point(i);const bool connected=previous>=0&&(market.stockTimeline?market.stockTimeline->rows[i].connectsPrevious:
            market.closed[i].startsAtMs-market.closed[previous].startsAtMs==market.intervalMs);
        if(connected)path.lineTo(p);else {path.moveTo(p);gap|=previous>=0;}previous=i;}
    painter.setPen(QPen(colors.rule,1));painter.drawLine(area.bottomLeft(),area.bottomRight());
    painter.setBrush(Qt::NoBrush);painter.setPen(QPen(movement,5,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));painter.drawPath(path);
    painter.setPen(Qt::NoPen);painter.setBrush(movement);painter.drawEllipse(point(visible.last()),6,6);
    if(gap){const auto label=QStringLiteral("자료 없는 구간은 연결하지 않아요");result.captions.append(label);text(painter,{68,518,864,24},label,FontRole::Caption12,colors.text3);}
    text(painter,{68,550,864,30},QStringLiteral("시장 가격 변화"),FontRole::BodyStrong15,colors.studyPurple);
    text(painter,{68,588,864,112},result.heroText,FontRole::Hero44,movement,Qt::AlignLeft|Qt::AlignVCenter,2.0);
    text(painter,{68,708,864,64},change,FontRole::Lead28,movement);
    text(painter,{68,800,410,28},QStringLiteral("시작"),FontRole::Body15,colors.text2);
    text(painter,{522,800,410,28},QStringLiteral("현재"),FontRole::Body15,colors.text2);
    text(painter,{68,838,410,44},start,FontRole::Lead28,colors.text);
    text(painter,{522,838,410,44},current,FontRole::Lead28,colors.text);
    text(painter,{68,901,864,26},period,FontRole::Secondary14,colors.text2);
    text(painter,{68,936,864,26},source,FontRole::Secondary14,colors.text2);
    result.captions.append(personalRows);
    for(int i=0;i<personalRows.size();++i)text(painter,{68,qreal(974+i*26),864,24},personalRows[i],FontRole::Caption12,colors.text2);
    text(painter,{68,1120,600,28},privacy,FontRole::Caption12,colors.text3);
    text(painter,{708,1110,224,48},productName(),FontRole::Logo22,colors.studyPurple,Qt::AlignRight|Qt::AlignVCenter);
    painter.end();result.image=std::move(image);result.available=true;result.reason.clear();return result;
}
bool AssetShareCard::savePng(const AssetShareSnapshot& value, const AssetShareOptions& options, const QString& path, QString* error) {
    const auto reject=[&](){if(error)*error=QStringLiteral("PNG를 명시한 로컬 파일에 저장하지 못했습니다.");return false;};
    if(path.isEmpty()||path.contains(QChar(0))||!QDir::isAbsolutePath(path)||path.contains("://")||
       QFileInfo(path).suffix().toLower()!="png"||QFileInfo(path).isDir())return reject();
    const auto rendered=render(value,options);if(!rendered.available)return reject();
    QSaveFile file(path);file.setDirectWriteFallback(false);if(!file.open(QIODevice::WriteOnly))return reject();
    QImageWriter writer(&file,"png");if(!writer.write(rendered.image)){file.cancelWriting();return reject();}
    if(!file.commit())return reject();if(error)error->clear();return true;
}
}
