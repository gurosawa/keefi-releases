#include "keefi/OkxProtocol.h"
#include <QJsonArray>

namespace keefi {
namespace {
bool number(const QString& value, bool positive = false) {
    static const QRegularExpression re(QStringLiteral("\\A[0-9]{1,80}(?:\\.[0-9]{1,80})?\\z"));
    return re.match(value).hasMatch() && (!positive || decimal::positive(value));
}
bool millis(const QString& text) {
    static const QRegularExpression re(QStringLiteral("\\A[0-9]{1,19}\\z"));
    bool ok = false; const auto value = text.toLongLong(&ok);
    return re.match(text).hasMatch() && ok && value > 0;
}
}
bool parseOkxOrder(const QJsonObject& o, OkxOrderSource source, OrderRecord* output, QString* error) {
    if (error) error->clear();
    const auto fail = [&] { if (error) *error = QStringLiteral("OKX 주문 필수 값·단위를 확인하지 못했습니다."); return false; };
    if (!output) return fail();
    for (const auto* key : {"ordId", "instId", "instType", "tdMode", "side", "ordType", "state", "sz", "accFillSz", "uTime"})
        if (!o.value(key).isString()) return fail();
    const auto text = [&](const char* key) { return o.value(key).toString(); };
    for (const auto* key : {"clOrdId", "px", "tgtCcy", "algoId", "algoClOrdId", "attachAlgoClOrdId", "tpTriggerPx", "slTriggerPx", "category", "source"})
        if (o.contains(key) && !o.value(key).isString()) return fail();
    OrderRecord r;
    r.orderId = text("ordId"); r.clientId = text("clOrdId"); r.symbol = text("instId");
    r.instType = text("instType"); r.tdMode = text("tdMode"); r.side = text("side"); r.type = text("ordType");
    r.rawStatus = text("state"); r.sourceUpdatedAt = text("uTime");
    if (!okxOrderIdValid(r.orderId) || !okxClientIdValid(r.clientId) || !okxInstrumentValid(r.symbol) ||
        r.instType != "SPOT" || !QStringList{"cash", "cross", "isolated"}.contains(r.tdMode) ||
        !QStringList{"buy", "sell"}.contains(r.side) || !QRegularExpression("\\A[a-z_]{1,64}\\z").match(r.type).hasMatch() ||
        !millis(r.sourceUpdatedAt) || !number(text("sz"), true) || !number(text("accFillSz"))) return fail();
    const auto assets = r.symbol.split('-'); r.baseAsset = assets[0]; r.quoteAsset = assets[1];
    r.currency = r.quoteAsset; r.market = "spot";
    const QMap<QString, QString> statuses{{"live", "open"}, {"partially_filled", "partial"}, {"filled", "filled"},
        {"canceled", "canceled"}, {"mmp_canceled", "canceled"}};
    r.status = statuses.value(r.rawStatus, "unknown");
    if (r.rawStatus.size() > 64 || !QRegularExpression("\\A[a-z_]{1,64}\\z").match(r.rawStatus).hasMatch()) return fail();
    r.executed = r.executedBaseAmount = text("accFillSz");
    r.price = text("px"); if (!r.price.isEmpty() && !number(r.price)) return fail();
    const bool market = r.type == "market";
    if (market) { if (!r.price.isEmpty() && decimal::compare(r.price, "0") != 0) return fail(); r.price.clear(); }
    auto target = text("tgtCcy");
    if (market && target.isEmpty()) target = r.side == "buy" ? "quote_ccy" : "base_ccy";
    if (market && target != "quote_ccy" && target != "base_ccy") return fail();
    r.quantityUnit = market && target == "quote_ccy" ? "quote" : "base";
    if (r.quantityUnit == "quote") {
        r.quoteAmount = r.requestedQuoteAmount = text("sz");
        // quote sz minus base accFillSz is dimensionally invalid. Unknown stays empty.
    } else {
        r.quantity = text("sz");
        if (decimal::compare(r.quantity, r.executedBaseAmount) >= 0)
            r.remaining = r.remainingBaseAmount = decimal::subtract(r.quantity, r.executedBaseAmount);
    }
    if (!market && !number(r.price, true)) return fail();
    for (const auto* key : {"algoId", "algoClOrdId", "attachAlgoClOrdId", "tpTriggerPx", "slTriggerPx"}) {
        if (text(key).size() > 256) return fail();
        if (!text(key).isEmpty()) r.linkedAlgo = true;
    }
    if (o.contains("attachAlgoOrds")) {
        if (!o.value("attachAlgoOrds").isArray() || o.value("attachAlgoOrds").toArray().size() > 100) return fail();
        r.linkedAlgo |= !o.value("attachAlgoOrds").toArray().isEmpty();
    }
    if (o.contains("linkedAlgoOrd")) {
        if (!o.value("linkedAlgoOrd").isObject()) return fail();
        const auto linked = o.value("linkedAlgoOrd").toObject();
        if (linked.contains("algoId") && !linked.value("algoId").isString()) return fail();
        r.linkedAlgo |= !linked.value("algoId").toString().isEmpty();
    }
    r.linkedAlgo |= QStringList{"6", "7", "13", "25", "34"}.contains(text("source"));
    const bool normal = text("category").isEmpty() || text("category") == "normal";
    const bool open = r.status == "open" || r.status == "partial";
    const bool supported = QStringList{"limit", "market", "post_only", "ioc", "fok"}.contains(r.type);
    r.cancelable = open && supported && normal && r.tdMode == "cash" && !r.linkedAlgo;
    r.amendable = r.cancelable && r.type == "limit" && decimal::positive(r.remainingBaseAmount);
    if (source == OkxOrderSource::WebSocket) {
        for (const auto* key : {"reqId", "amendResult"}) if (o.contains(key) && !o.value(key).isString()) return fail();
        if (!okxClientIdValid(text("reqId")) || !QStringList{"", "0", "-1"}.contains(text("amendResult")) ||
            (text("reqId").isEmpty() && !text("amendResult").isEmpty())) return fail();
        r.amendRequestId = text("reqId");
    }
    // fillSz/fillFee are event-local for WS and differ from REST. No cumulative
    // quantities or fees are synthesized from these fields.
    *output = std::move(r); return true;
}
} // namespace keefi
