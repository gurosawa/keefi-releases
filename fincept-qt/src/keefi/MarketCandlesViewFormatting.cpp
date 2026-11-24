#include "keefi/MarketCandlesViewFormatting_p.h"
#include "keefi/MarketCandlesView.h"
#include "keefi/ChartStudyHelp.h"
#include "keefi/KeefiTheme.h"
#include "keefi/MovementColorPolicy.h"
#include <QDateTime>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QTimeZone>
#include <cmath>

namespace keefi::market_candles_detail {
QString localTime(qint64 at){return QDateTime::fromMSecsSinceEpoch(at,QTimeZone("Asia/Seoul")).toString("MM/dd HH:mm");}
QString sourceTime(qint64 at){return at>0?QDateTime::fromMSecsSinceEpoch(at,QTimeZone("Asia/Seoul")).toString("yyyy-MM-dd HH:mm:ss")+" KST":QObject::tr("미확인");}
QString timeframeLabel(qint64 interval){
    if(interval%86400000==0)return QObject::tr("%1일").arg(interval/86400000);
    if(interval%3600000==0)return QObject::tr("%1시간").arg(interval/3600000);
    return QObject::tr("%1분").arg(interval/60000);
}
bool supportedTimeframe(qint64 interval){return interval==60000||interval==300000||interval==900000||interval==3600000||interval==14400000||interval==86400000;}
QColor defaultStudyColor(const QString& key, const QObject* context) {
    const auto& colors=themeColors();
    if(key=="bbUpper" || key=="bbLower" || key=="low_volume")return colors.text3;
    if(key=="ma2" || key=="ma4" || key=="bbMiddle" || key=="macdSignal" || key=="similarity" || key=="similarity2")return colors.studyBlue;
    if(key=="similarity3")return colors.studyOrange;
    if(key=="pullback")return colors.accent;
    if(key=="levels")return colors.level;
    if(key.endsWith("Up") || key.endsWith("Down")){
        const auto direction=key.endsWith("Up")?MovementDirection::Up:MovementDirection::Down;
        return key.startsWith("macdHistogram")?MovementColorPolicy::colorFor(context,direction):MovementColorPolicy::fillFor(context,direction);
    }
    return colors.studyPurple;
}
QString studyName(const QString& id){return chartStudyName(id);}
QString accountLabel(const QList<ChartPriceLine>& rows,const QString& scope){
    QStringList scopes;for(const auto& row:rows)if(!scopes.contains(row.accountScope))scopes.append(row.accountScope);
    return QObject::tr("계좌 %1").arg(scopes.indexOf(scope)+1);
}
QString drawnPrice(double price){
    if(!std::isfinite(price)||price<=0||price<1e-100||price>1e100)return {};
    auto text=QString::number(price,'f',qBound(0,14-int(std::floor(std::log10(price))),110));
    if(text.contains('.')){while(text.endsWith('0'))text.chop(1);if(text.endsWith('.'))text.chop(1);}
    return text;
}
QIcon chartLineIcon(const QString& kind,const QColor& ink,bool active){
    QPixmap pixels(48,48);pixels.setDevicePixelRatio(2);pixels.fill(Qt::transparent);QPainter p(&pixels);
    p.setRenderHint(QPainter::Antialiasing);p.setPen(QPen(ink,1.5,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.setBrush(Qt::NoBrush);
    if(kind.endsWith("crosshair")){p.drawLine(12,3,12,21);p.drawLine(3,12,21,12);p.drawEllipse(QPointF(12,12),3,3);}
    else if(kind.endsWith("trend")){p.drawLine(5,18,19,6);p.drawEllipse(QPointF(5,18),2,2);p.drawEllipse(QPointF(19,6),2,2);}
    else if(kind.endsWith("horizontal")){p.drawLine(3,12,21,12);p.drawEllipse(QPointF(12,12),2,2);}
    else if(kind.endsWith("measure")){p.drawLine(5,19,19,5);p.drawLine(5,14,5,19);p.drawLine(5,19,10,19);p.drawLine(14,5,19,5);p.drawLine(19,5,19,10);p.drawLine(10,10,14,14);}
    else if(kind.endsWith("Magnet")){QPainterPath path;path.moveTo(5,5);path.lineTo(5,13);path.cubicTo(5,22,19,22,19,13);path.lineTo(19,5);path.lineTo(15,5);path.lineTo(15,13);path.cubicTo(15,17,9,17,9,13);path.lineTo(9,5);path.closeSubpath();p.drawPath(path);p.drawLine(5,9,9,9);p.drawLine(15,9,19,9);}
    else if(kind.endsWith("Locked")){p.drawRoundedRect(QRectF(5,10,14,11),1,1);p.drawArc(QRectF(8,3,8,12),0,180*16);p.drawLine(12,14,12,17);}
    else if(kind.endsWith("Clear")){p.drawLine(5,6,19,6);p.drawLine(9,3,15,3);p.drawPolyline(QPolygonF{QPointF(7,6),QPointF(8,21),QPointF(16,21),QPointF(17,6)});p.drawLine(10,10,10,17);p.drawLine(14,10,14,17);}
    else if(kind=="off"){p.drawLine(6,6,18,18);p.drawLine(18,6,6,18);}
    else if(kind.endsWith("Undo")||kind.endsWith("Redo")){
        if(kind.endsWith("Redo")){p.translate(24,0);p.scale(-1,1);}
        QPainterPath path;path.moveTo(6,10);path.lineTo(15,10);path.cubicTo(21,10,21,19,15,19);path.lineTo(9,19);p.drawPath(path);p.drawLine(6,10,10,6);p.drawLine(6,10,10,14);}
    else if(kind=="style-candles"){p.drawLine(8,3,8,21);p.drawRect(QRectF(5.5,7,5,9));p.drawLine(16,5,16,19);p.setBrush(ink);p.drawRect(QRectF(13.5,9,5,6));}
    else if(kind=="style-bars"){p.drawLine(8,4,8,20);p.drawLine(4,8,8,8);p.drawLine(8,16,12,16);p.drawLine(16,6,16,18);p.drawLine(12,15,16,15);p.drawLine(16,9,20,9);}
    else if(kind=="style-line"){p.drawPolyline(QPolygonF{QPointF(3,17),QPointF(8,11),QPointF(13,14),QPointF(21,5)});}
    else {QPainterPath path;path.moveTo(2,12);path.cubicTo(8,3,16,3,22,12);path.cubicTo(16,21,8,21,2,12);p.drawPath(path);p.drawEllipse(QPointF(12,12),3,3);if(!active)p.drawLine(4,3,20,21);}
    return QIcon(pixels);
}

} // namespace keefi::market_candles_detail
