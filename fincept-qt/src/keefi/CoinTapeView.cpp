#include "keefi/CoinTapeView.h"
#include "keefi/CoinFlowSource.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/Motion.h"
#include "keefi/MovementColorPolicy.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/TradingTypes.h"
#include <QDateTime>
#include <QPaintEvent>
#include <QPainter>
#include <QSet>
#include <QTimeZone>
#include <QTimer>
#include <QVariantAnimation>
#include <algorithm>

namespace keefi {
namespace {
QString tapeTime(qint64 at) {
    return QDateTime::fromMSecsSinceEpoch(at, QTimeZone("Asia/Seoul")).toString(QStringLiteral("HH:mm:ss"));
}
bool coinChart(const QString& identity) {
    return identity.startsWith(QStringLiteral("coin:upbit:")) || identity.startsWith(QStringLiteral("coin:binance:"));
}
QString roundDecimals(const QString& exact, int places) {
    const QString half = QStringLiteral("0.") + QString(places, QLatin1Char('0')) + QLatin1Char('5');
    auto value = decimal::add(exact, half);
    const auto dot = value.indexOf('.');
    if (dot >= 0) value = value.left(dot + 1 + places);
    return priceAmount(value);
}
} // namespace

CoinTapeView::CoinTapeView(QWidget* parent) : QWidget(parent) {
    setObjectName("chartCoinTape");
    setAccessibleName(tr("체결·청산"));
    setFocusPolicy(Qt::NoFocus);
    setMinimumHeight(RowHeight * 3);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setSingleShot(true);
    connect(refreshTimer_, &QTimer::timeout, this, &CoinTapeView::refresh);
    flash_ = expoMotion(this, FlashMs);
    const auto repaintFlash = [this] {
        QRegion region;
        for (int i = 0; i < rows_.size() && i * RowHeight < height(); ++i)
            if (flashKeys_.contains(rows_[i].key)) region += rowRect(i);
        if (!region.isEmpty()) update(region);
    };
    connect(flash_, &QVariantAnimation::valueChanged, this, [this, repaintFlash](const QVariant& value) {
        flashProgress_ = value.toReal();
        repaintFlash();
    });
    connect(flash_, &QVariantAnimation::finished, this, [this, repaintFlash] {
        flashProgress_ = 1.0;
        repaintFlash();
        flashKeys_.clear();
    });
    bindThemePresentation(this, [this] { update(); });
    bindMovementPresentation(this, [this] { update(); });
}

void CoinTapeView::setSource(CoinFlowSource* source) {
    if (source_ == source) return;
    if (source_) disconnect(source_, nullptr, this, nullptr);
    source_ = source;
    if (source) {
        connect(source, &CoinFlowSource::changed, this, &CoinTapeView::scheduleRefresh);
        connect(source, &QObject::destroyed, this, [this] { rows_.clear(); cache_.clear(); update(); });
    }
    rows_.clear();
    cache_.clear();
    largeKey_.clear();
    largeIds_.clear();
    update();
    scheduleRefresh();
}

void CoinTapeView::setIdentity(const QString& identity) {
    if (identity_ == identity) return;
    identity_ = identity;
    flash_->stop();
    flashKeys_.clear();
    rows_.clear();
    cache_.clear();
    largeKey_.clear();
    largeIds_.clear();
    update();
    scheduleRefresh();
}

QString CoinTapeView::emptyText() const {
    return coinChart(identity_) ? tr("체결 수신 대기") : tr("코인 차트에서 보여요");
}

bool CoinTapeView::flashing() const { return flash_->state() == QAbstractAnimation::Running; }

QString CoinTapeView::venueName(CoinFlowVenue venue) {
    switch (venue) {
    case CoinFlowVenue::UpbitSpot: return tr("업비트");
    case CoinFlowVenue::BinanceSpot: return tr("바이낸스");
    case CoinFlowVenue::BinanceUsdM: return tr("바이낸스 .P");
    }
    return {};
}

QString CoinTapeView::amountText(const QString& notional, const QString& quote) {
    if (!decimal::valid(notional) || quote.isEmpty()) return QStringLiteral("—");
    const bool positive = decimal::positive(notional);
    if (quote == QStringLiteral("KRW")) {
        if (positive && decimal::compare(notional, QStringLiteral("0.5")) < 0)
            return QStringLiteral("<1") + tr("원");
        return krwAmount(notional) + tr("원");
    }
    if (!positive) return QStringLiteral("0 ") + quote;
    if (decimal::compare(notional, QStringLiteral("1000")) >= 0)
        return krwAmount(notional) + ' ' + quote;
    if (decimal::compare(notional, QStringLiteral("1")) >= 0)
        return roundDecimals(notional, 2) + ' ' + quote;
    const bool stable = quote == QStringLiteral("USDT") || quote == QStringLiteral("USDC") || quote == QStringLiteral("USD");
    if (stable) {
        if (decimal::compare(notional, QStringLiteral("0.0001")) < 0)
            return QStringLiteral("<0.0001 ") + quote;
        return roundDecimals(notional, 4) + ' ' + quote;
    }
    if (decimal::compare(notional, QStringLiteral("0.0001")) >= 0)
        return roundDecimals(notional, 6) + ' ' + quote;
    if (decimal::compare(notional, QStringLiteral("0.00000001")) >= 0)
        return roundDecimals(notional, 8) + ' ' + quote;
    return QStringLiteral("<0.00000001 ") + quote;
}

void CoinTapeView::scheduleRefresh() {
    if (!isVisible() || refreshTimer_->isActive()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    refreshTimer_->start(int(qBound<qint64>(0, qint64(RefreshMs) - (now - lastRefreshMs_), qint64(RefreshMs))));
}

void CoinTapeView::refresh() {
    lastRefreshMs_ = QDateTime::currentMSecsSinceEpoch();
    showSnapshot(source_ && coinChart(identity_) ? source_->snapshot() : CoinFlowSnapshot{});
}

void CoinTapeView::showSnapshot(const CoinFlowSnapshot& snapshot) {
    QList<Row> next;
    const auto& instrument = snapshot.primary.instrument;
    if (coinChart(identity_) && snapshot.identity == identity_ && instrument.verified && instrument.identity == identity_) {
        const auto& trades = snapshot.primary.trades;
        const qsizetype first = qMax<qsizetype>(0, trades.size() - MaxRows);
        // Judge the window as of everything the snapshot already holds.
        const auto& periods = snapshot.primary.periods;
        qint64 asOf = qMax(QDateTime::currentMSecsSinceEpoch(), instrument.evidence.knownAtMs);
        for (const auto& trade : trades) asOf = qMax(asOf, trade.knownAtMs);
        for (const auto& period : periods) asOf = qMax(asOf, period.knownAtMs);
        const QString key = identity_ + '|' + QString::number(periods.size()) + '|' +
            (periods.isEmpty() ? QString{} : QString::number(periods.last().throughMs) + ':' + QString::number(periods.last().knownAtMs)) +
            '|' + (trades.isEmpty() ? QString{} : trades.first().id);
        if (key != largeKey_) {
            largeKey_ = key;
            largeIds_.clear();
            const qint64 from = first < trades.size() ? trades.at(first).tradeAtMs : 0;
            for (const auto& point : coinLargeTrades(snapshot.primary, asOf, from))
                largeIds_.insert(point.id + ':' + QString::number(point.tradeAtMs));
        }
        const auto venue = venueName(instrument.venue);
        for (qsizetype i = trades.size() - 1; i >= first; --i) {
            const auto& trade = trades.at(i);
            if (trade.snapshot) continue;
            const QString rowKey = QStringLiteral("t:") + trade.id;
            auto found = cache_.constFind(rowKey);
            Row row;
            if (found != cache_.cend()) row = found.value();
            else {
                row.key = rowKey;
                row.venue = venue;
                row.atMs = trade.tradeAtMs;
                row.side = trade.side == CoinFlowSide::Buy ? MovementDirection::Up : MovementDirection::Down;
                row.price = priceAmount(trade.price);
                row.amount = amountText(decimal::multiply(trade.price, trade.quantity), instrument.quoteAsset);
            }
            row.large = largeIds_.contains(trade.id + ':' + QString::number(trade.tradeAtMs));
            next.append(row);
        }
        // Same-coin USD-M liquidations: the order's own side, averagePrice ×
        // cumulative filled quantity (as in the existing liquidation records).
        if (snapshot.companion) {
            const auto& liquidations = snapshot.liquidations;
            const qsizetype start = qMax<qsizetype>(0, liquidations.size() - MaxRows);
            for (qsizetype i = start; i < liquidations.size(); ++i) {
                const auto& value = liquidations.at(i);
                const QString rowKey = QStringLiteral("l:") + value.symbol + '|' + value.side + '|' + QString::number(value.tradeAtMs) + '|' +
                    QString::number(value.eventAtMs) + '|' + value.averagePrice + '|' + value.cumulativeFilledQuantity;
                auto found = cache_.constFind(rowKey);
                Row row;
                if (found != cache_.cend()) row = found.value();
                else {
                    const auto price = decimal::positive(value.averagePrice) ? value.averagePrice : value.price;
                    row.key = rowKey;
                    row.venue = venueName(CoinFlowVenue::BinanceUsdM);
                    row.atMs = value.tradeAtMs;
                    row.liquidation = true;
                    row.side = value.side == QStringLiteral("BUY") ? MovementDirection::Up
                        : value.side == QStringLiteral("SELL") ? MovementDirection::Down : MovementDirection::Unknown;
                    row.price = priceAmount(price);
                    row.amount = amountText(decimal::multiply(price, value.cumulativeFilledQuantity), snapshot.companion->instrument.quoteAsset);
                }
                next.append(row);
            }
            std::stable_sort(next.begin(), next.end(), [](const Row& a, const Row& b) { return a.atMs > b.atMs; });
            if (next.size() > MaxRows) next.resize(MaxRows);
        }
    }
    QStringList fresh;
    if (!rows_.isEmpty()) {
        QSet<QString> previous;
        for (const auto& row : rows_) previous.insert(row.key);
        for (const auto& row : next)
            if (!previous.contains(row.key)) fresh.append(row.key);
    }
    QHash<QString, Row> kept;
    for (const auto& row : next) kept.insert(row.key, row);
    cache_ = kept;
    rows_ = next;
    // Only the newest arrivals flash; a refresh without one leaves a running
    // flash alone. Reduced motion and a hidden tape show the final frame.
    if (!fresh.isEmpty()) {
        flash_->stop();
        flashKeys_.clear();
        flashProgress_ = 1.0;
        if (motionEnabled() && isVisible()) {
            flashKeys_ = fresh;
            flashProgress_ = 0.0;
            flash_->start();
        }
    }
    update();
}

QRect CoinTapeView::rowRect(int index) const { return QRect(0, index * RowHeight, width(), RowHeight); }

void CoinTapeView::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto& colors = themeColors();
    if (rows_.isEmpty()) {
        painter.setPen(colors.text3);
        painter.setFont(KeefiFonts::font(FontRole::Secondary14));
        painter.drawText(rect().adjusted(12, 0, -12, 0), Qt::AlignCenter | Qt::TextWordWrap, emptyText());
        return;
    }
    const QFont number = KeefiFonts::font(FontRole::Number11), caption = KeefiFonts::font(FontRole::Caption12);
    const QFontMetrics numberMetrics(number), captionMetrics(caption);
    constexpr int padding = 10, gap = 6;
    const int visible = qMin<qsizetype>(rows_.size(), (height() + RowHeight - 1) / RowHeight);
    const int timeWidth = numberMetrics.horizontalAdvance(QStringLiteral("00:00:00"));
    const int badgeWidth = captionMetrics.horizontalAdvance(tr("매수")) + 12;
    int amountWidth = 0;
    for (int i = 0; i < visible; ++i) amountWidth = qMax(amountWidth, numberMetrics.horizontalAdvance(rows_[i].amount) + 4);
    const int room = width() - 2 * padding - timeWidth - badgeWidth - 3 * gap - 2;
    amountWidth = qMin(amountWidth, room * 3 / 5);
    const int middle = qMax(0, room - amountWidth);
    for (int i = 0; i < visible; ++i) {
        const QRect line = rowRect(i);
        if (!event->rect().intersects(line)) continue;
        const auto& row = rows_[i];
        const QColor tone = row.side == MovementDirection::Unknown ? colors.text2 : MovementColorPolicy::colorFor(this, row.side);
        if (flashProgress_ < 1.0 && flashKeys_.contains(row.key)) {
            QColor fill = tone;
            fill.setAlpha(qRound(64 * (1.0 - flashProgress_)));
            painter.fillRect(line, fill);
        }
        painter.setPen(colors.rule);
        painter.drawLine(line.left() + padding, line.bottom(), line.right() - padding, line.bottom());
        int x = line.left() + padding;
        painter.setFont(number);
        painter.setPen(colors.text3);
        painter.drawText(QRect(x, line.top(), timeWidth, line.height()), Qt::AlignLeft | Qt::AlignVCenter, tapeTime(row.atMs));
        x += timeWidth + gap;
        const auto tag = row.liquidation ? tr(" · 청산") : row.large ? tr(" · 상위 1%") : QString{};
        painter.setPen(row.large || row.liquidation ? colors.text : colors.text2);
        drawLedgerText(painter, QRectF(x, line.top() + 2, middle, line.height() / 2.0 - 1), row.venue + tag, FontRole::Caption12);
        painter.setFont(number);
        painter.setPen(tone);
        const QRect price(x, line.top() + line.height() / 2, middle, line.height() / 2 - 3);
        painter.drawText(price, Qt::AlignLeft | Qt::AlignVCenter, numberMetrics.elidedText(row.price, Qt::ElideRight, price.width()));
        x += middle + gap;
        // Digits in the number face, 원/USDT in the body face, as everywhere else.
        painter.setPen(row.large || row.liquidation ? colors.text : colors.text2);
        drawLedgerText(painter, QRectF(x, line.top(), amountWidth, line.height()), row.amount, FontRole::Caption12, Qt::AlignRight);
        x += amountWidth + gap + 2;
        if (row.side == MovementDirection::Unknown) continue;
        const QRectF badge(x, line.center().y() - 10, badgeWidth, 20);
        QColor tint = tone;
        tint.setAlpha(36);
        painter.setPen(Qt::NoPen);
        painter.setBrush(tint);
        painter.drawRoundedRect(badge, 4, 4);
        painter.setFont(caption);
        painter.setPen(tone);
        painter.drawText(badge, Qt::AlignCenter, row.side == MovementDirection::Up ? tr("매수") : tr("매도"));
    }
}

void CoinTapeView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // The first frame already shows what the source holds now. Rows that came
    // while the tape was hidden were never seen arriving, so none of them flashes.
    refreshTimer_->stop();
    refresh();
    emit shownChanged(true);
}

void CoinTapeView::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    refreshTimer_->stop();
    flash_->stop();
    flashKeys_.clear();
    flashProgress_ = 1.0;
    // A hidden tape keeps no rows; the next show starts from a fresh fill.
    rows_.clear();
    cache_.clear();
    emit shownChanged(false);
}
} // namespace keefi
