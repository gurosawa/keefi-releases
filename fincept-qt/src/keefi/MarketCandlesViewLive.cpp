#include "keefi/MarketCandlesView.h"
#include "keefi/CandleChart_p.h"

namespace keefi {

void MarketCandlesView::refreshLiveObservation() {
    auto* chart = static_cast<market_candles_detail::CandleChart*>(chart_);
    if (!chart) return;
    const auto clear = [chart] { chart->setLiveTradePrice({}, 0); };
    if (!model_ || !isVisible() || !identity_.startsWith("coin:") ||
        chart->property("displayIdentity").toString() != identity_ ||
        chart->property("displayIntervalMs").toLongLong() != intervalMs_ ||
        chart->property("displayHistoricalOnly").toBool()) {
        clear();
        return;
    }

    const auto official = model_->series();
    const auto display = model_->displayObservation();
    if (!official.available || official.identity != identity_ ||
        official.intervalMs != intervalMs_ || official.currency != currency_ ||
        !display.available || display.delayed || display.fromArchive ||
        display.fromMemoryCache || display.identity != identity_ ||
        display.intervalMs != intervalMs_ || display.currency != currency_) {
        clear();
        return;
    }

    // This deliberately bypasses studies, information panels and drawing/alert
    // evidence. The model coalesces trade notifications; every trade is still
    // validated/accumulated before the latest display observation is obtained.
    chart->setObservation(display.candles, display.forming);
    const auto live = model_->liveTradeObservation();
    if (live.available && live.identity == identity_ && live.intervalMs == intervalMs_)
        chart->setLiveTradePrice(live.price, live.tradeAtMs);
    else
        clear();
}

} // namespace keefi
