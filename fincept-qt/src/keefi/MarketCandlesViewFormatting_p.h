#pragma once

#include <QColor>
#include <QIcon>
#include <QList>
#include <QString>

class QObject;

namespace keefi {
struct ChartPriceLine;
namespace market_candles_detail {
inline constexpr int CandlePlotLeft=8;
inline constexpr int CandlePlotTop=12;
inline constexpr int CandlePriceAxisMinimum=75;
inline constexpr int CandleOhlcInset=2;
inline constexpr int CandleOhlcHeight=18;
inline constexpr int StudyLegendPadding=4;
inline constexpr int StudyLegendChipGap=6;
inline constexpr int StudyLegendMoreGap=8;
inline constexpr int StudyChipLeadingPadding=8;
inline constexpr int StudyChipInfoMarkWidth=16;

QString localTime(qint64 at);
QString sourceTime(qint64 at);
QString timeframeLabel(qint64 interval);
bool supportedTimeframe(qint64 interval);
QColor defaultStudyColor(const QString& key, const QObject* context);
QString studyName(const QString& id);
QString accountLabel(const QList<ChartPriceLine>& rows, const QString& scope);
QString drawnPrice(double price);
QIcon chartLineIcon(const QString& kind, const QColor& ink, bool active = true);
} // namespace market_candles_detail
} // namespace keefi
