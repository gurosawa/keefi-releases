#include "keefi/ChartMarketContext.h"
#include "keefi/PortfolioPresentation.h"
#include <QDateTime>
#include <QObject>
#include <QRegularExpression>
#include <QStringList>
#include <QTimeZone>
#include <cmath>
#include <optional>

namespace keefi {
namespace {
std::optional<double> positive(const QString& text) {
    static const QRegularExpression pattern(QStringLiteral("\\A[0-9]{1,15}(?:\\.[0-9]{1,12})?\\z"));
    if (!pattern.match(text).hasMatch()) return std::nullopt;
    bool ok = false;
    const double value = text.toDouble(&ok);
    if (!ok || !std::isfinite(value) || value <= 0) return std::nullopt;
    return value;
}
QString signedPercent(double value) {
    auto text = QString::number(value, 'f', 2);
    if (text == QStringLiteral("-0.00")) text = QStringLiteral("0.00");
    return (value > 0 && text != QStringLiteral("0.00") ? QStringLiteral("+") : QString{}) + text + QLatin1Char('%');
}
MovementDirection directionOf(const QString& signedDecimal) {
    bool ok = false;
    const double value = signedDecimal.toDouble(&ok);
    if (signedDecimal.isEmpty() || !ok || !std::isfinite(value)) return MovementDirection::Unknown;
    return value > 0 ? MovementDirection::Up : value < 0 ? MovementDirection::Down : MovementDirection::Flat;
}
QString kst(qint64 ms, const QString& format) { return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone("Asia/Seoul")).toString(format); }
bool fresh(qint64 atMs, qint64 nowMs) { return atMs > 0 && atMs <= nowMs + 5000 && nowMs - atMs <= chartmarket::PriceFreshnessMs; }

ChartMarketItem quoteItem(const QString& label, WorldMarketItem market, const WorldMarketsSnapshot& world, qint64 now, bool showValue) {
    ChartMarketItem item;
    const auto& quote = world.quote(market);
    if (!quote.available()) return item;
    item.label = label;
    if (showValue) item.value = groupedAmount(quote.price);
    if (quote.session == WorldMarketSession::PreOpen) item.change = QObject::tr("장 전");
    else {
        item.direction = directionOf(quote.changePercent);
        bool ok = false;
        const double percent = quote.changePercent.toDouble(&ok);
        if (ok && std::isfinite(percent)) item.change = signedPercent(percent);
    }
    item.closed = quote.session == WorldMarketSession::Closed;
    const bool closedIndex = item.closed && market >= WorldMarketItem::Spx && market <= WorldMarketItem::Ftse;
    item.late = now - quote.receivedAtMs > 3 * (closedIndex ? WorldMarkets::ClosedRefreshMs : WorldMarkets::RefreshMs);
    QStringList detail{label + QStringLiteral(" ") + groupedAmount(quote.price)};
    if (!item.change.isEmpty()) detail.append(item.change);
    detail.append(QObject::tr("기준 %1 KST").arg(kst(quote.tradedAtMs, QStringLiteral("MM/dd HH:mm"))));
    if (item.closed) detail.append(QObject::tr("장 마감"));
    if (quote.delayMinutes > 0) detail.append(QObject::tr("%1분 지연").arg(quote.delayMinutes));
    if (item.late) detail.append(QObject::tr("최근 확인 %1").arg(kst(quote.receivedAtMs, QStringLiteral("HH:mm"))));
    item.detail = detail.join(QStringLiteral(" · "));
    return item;
}
ChartMarketItem dollarIndexItem(const WorldDollarIndex& index, qint64 now) {
    ChartMarketItem item;
    if (!index.available()) return item;
    item.label = QObject::tr("달러 인덱스");
    item.value = index.value;
    if (!index.changePercent.isEmpty()) {
        item.direction = directionOf(index.changePercent);
        bool ok = false;
        const double percent = index.changePercent.toDouble(&ok);
        if (ok && std::isfinite(percent)) item.change = signedPercent(percent);
    }
    item.late = now - index.receivedAtMs > 3 * WorldMarkets::DailyRefreshMs;
    item.detail = QObject::tr("달러 인덱스 %1 · ECB %2 일별 환율로 계산한 근사값").arg(index.value, index.date.toString(Qt::ISODate));
    return item;
}
} // namespace

QString chartKrwAmount(double won) {
    if (!std::isfinite(won) || won <= 0) return {};
    if (won >= 100) return groupedAmount(QString::number(static_cast<qint64>(std::llround(won))));
    if (won >= 1) return groupedAmount(QString::number(won, 'f', 2));
    const int decimals = qBound(2, 3 - int(std::floor(std::log10(won))), 10);
    auto text = QString::number(won, 'f', decimals);
    while (text.endsWith(QLatin1Char('0')) && !text.endsWith(QStringLiteral(".0"))) text.chop(1);
    if (text.endsWith(QStringLiteral(".0"))) text.chop(2);
    return text;
}

ChartMarketContext chartMarketContext(const ChartMarketInputs& in) {
    ChartMarketContext out;
    if (in.market.isEmpty() || in.code.isEmpty()) return out;
    out.identityKey = in.market + QLatin1Char(':') + in.source + QLatin1Char(':') + in.code;
    const auto& fx = in.world.quote(WorldMarketItem::UsdKrw);
    const auto rate = fx.available() ? positive(fx.price) : std::nullopt;
    // A posting this app has not re-read in three minutes is not used for a live conversion.
    const bool rateFresh = rate && in.nowMs - fx.receivedAtMs <= 3 * WorldMarkets::RefreshMs;
    const auto basis = rateFresh ? QObject::tr("원/달러 %1 (하나은행 %2 고시)").arg(groupedAmount(fx.price), kst(fx.tradedAtMs, QStringLiteral("MM/dd HH:mm")))
                                 : QString{};
    const bool coin = in.market == QStringLiteral("coin");
    const auto price = positive(in.price);
    const bool dollar = in.currency == QStringLiteral("USD") || in.currency == QStringLiteral("USDT");
    if (rateFresh && price && dollar) {
        out.conversion = QObject::tr("≈ %1원").arg(chartKrwAmount(*price * *rate));
        out.conversionDetail = QObject::tr("원화 환산: %1 %2 × %3.").arg(groupedAmount(in.price), in.currency, basis)
            + (in.currency == QStringLiteral("USDT") ? QObject::tr(" 1 USDT ≈ 1 USD로 봅니다.") : QString{})
            + QObject::tr(" 은행 고시 환율이라 실제 환전·체결 가격과 다릅니다.");
    }
    const auto upbit = positive(in.upbitKrw), binance = positive(in.binanceUsdt);
    const bool upbitFresh = upbit && fresh(in.upbitAtMs, in.nowMs), binanceFresh = binance && fresh(in.binanceAtMs, in.nowMs);
    if (coin && rateFresh && in.currency == QStringLiteral("KRW") && binanceFresh) {
        out.conversion = QObject::tr("바이낸스 %1원").arg(chartKrwAmount(*binance * *rate));
        out.conversionDetail = QObject::tr("바이낸스 현물 %1 USDT × %2. 1 USDT ≈ 1 USD로 봅니다.").arg(groupedAmount(in.binanceUsdt), basis);
    }
    if (coin && rateFresh && upbitFresh && binanceFresh && qAbs(in.upbitAtMs - in.binanceAtMs) <= chartmarket::MaximumSkewMs) {
        const double premium = (*upbit / (*binance * *rate) - 1) * 100;
        if (std::isfinite(premium) && std::abs(premium) <= 50) {
            out.premium = QObject::tr("김프 %1").arg(signedPercent(premium));
            out.premiumDetail = QObject::tr("김프: (업비트 %1원 ÷ (바이낸스 %2 USDT × %3) − 1) × 100. 1 USDT ≈ 1 USD로 봅니다. 두 거래소 가격의 기준 시각은 1분 안에 있어야 표시합니다. "
                                            "오늘 화면의 BTC 김프와 같은 하나은행 기준입니다.")
                .arg(groupedAmount(in.upbitKrw), groupedAmount(in.binanceUsdt), basis);
        }
    }
    const auto add = [&](const ChartMarketItem& item) { if (!item.label.isEmpty()) out.related.append(item); };
    const auto& world = in.world;
    if (coin) {
        // Crypto has tended to move with US tech and against the dollar; KRW pairs also carry the won.
        add(quoteItem(QObject::tr("나스닥 100"), WorldMarketItem::Ndx, world, in.nowMs, false));
        add(dollarIndexItem(world.dollarIndex, in.nowMs));
        add(quoteItem(QObject::tr("원/달러"), WorldMarketItem::UsdKrw, world, in.nowMs, true));
    } else if (in.market == QStringLiteral("stock") && in.source == QStringLiteral("kr")) {
        add(quoteItem(QObject::tr("코스피"), WorldMarketItem::Kospi, world, in.nowMs, false));
        add(quoteItem(QObject::tr("원/달러"), WorldMarketItem::UsdKrw, world, in.nowMs, true));
        add(quoteItem(QObject::tr("나스닥 100"), WorldMarketItem::Ndx, world, in.nowMs, false));
    } else if (in.market == QStringLiteral("stock") && in.source == QStringLiteral("us")) {
        add(quoteItem(QStringLiteral("S&P 500"), WorldMarketItem::Spx, world, in.nowMs, false));
        add(quoteItem(QObject::tr("나스닥 100"), WorldMarketItem::Ndx, world, in.nowMs, false));
        add(quoteItem(QObject::tr("원/달러"), WorldMarketItem::UsdKrw, world, in.nowMs, true));
    }
    return out;
}

} // namespace keefi
