#pragma once
#include "keefi/TradingTypes.h"
#include <QRegularExpression>
#include <QSet>
#include <QUrlQuery>

namespace keefi {
// Read authority for G9-C1 deposit, withdrawal and wallet-transfer history.
// No withdrawal, transfer, pocket or order execution route is allowed here.
enum class FlowSource { UpbitDeposits, UpbitWithdrawals, BinanceDeposits, BinanceWithdrawals, BinanceTransfers };
inline bool flowUpbit(FlowSource s) { return s == FlowSource::UpbitDeposits || s == FlowSource::UpbitWithdrawals; }
inline QString flowPath(FlowSource s) {
    switch (s) {
    case FlowSource::UpbitDeposits: return "/v1/deposits";
    case FlowSource::UpbitWithdrawals: return "/v1/withdraws";
    case FlowSource::BinanceDeposits: return "/sapi/v1/capital/deposit/hisrec";
    case FlowSource::BinanceWithdrawals: return "/sapi/v1/capital/withdraw/history";
    case FlowSource::BinanceTransfers: return "/sapi/v1/asset/transfer";
    }
    return {};
}
// Universal transfer types that move value into or out of the Spot wallet
// (MAIN) or the USD-M Futures wallet (UMFUTURE), the C1 Binance scopes.
inline QStringList flowSpotTransferTypes() {
    return {"MAIN_UMFUTURE", "UMFUTURE_MAIN", "MAIN_CMFUTURE", "CMFUTURE_MAIN", "MAIN_MARGIN", "MARGIN_MAIN",
        "MAIN_FUNDING", "FUNDING_MAIN", "MAIN_OPTION", "OPTION_MAIN", "MAIN_PORTFOLIO_MARGIN", "PORTFOLIO_MARGIN_MAIN"};
}
inline QStringList flowUsdmTransferTypes() {
    return {"MAIN_UMFUTURE", "UMFUTURE_MAIN", "UMFUTURE_MARGIN", "MARGIN_UMFUTURE", "FUNDING_UMFUTURE",
        "UMFUTURE_FUNDING", "UMFUTURE_OPTION", "OPTION_UMFUTURE"};
}
inline bool isFundFlowRead(Operation op) { return op == Operation::FundFlowRead; }
inline bool allowedFundFlowRead(const RequestSpec& r) {
    if (!isFundFlowRead(r.operation) || !r.authenticated || r.method != "GET" || !r.body.isEmpty()) return false;
    const auto& u = r.url;
    if (!u.isValid() || u.scheme() != "https" || u.port(443) != 443 || !u.userInfo().isEmpty() || u.hasFragment()) return false;
    const bool upbit = u.host() == "api.upbit.com";
    if (!upbit && u.host() != "api.binance.com") return false;
    bool found = false; FlowSource source = FlowSource::UpbitDeposits;
    for (const auto s : {FlowSource::UpbitDeposits, FlowSource::UpbitWithdrawals, FlowSource::BinanceDeposits,
             FlowSource::BinanceWithdrawals, FlowSource::BinanceTransfers})
        if (u.path(QUrl::FullyEncoded) == flowPath(s) && flowUpbit(s) == upbit) { source = s; found = true; break; }
    if (!found) return false;
    QSet<QByteArray> headers;
    for (auto i = r.headers.cbegin(); i != r.headers.cend(); ++i) {
        const auto name = i.key().toLower();
        if (headers.contains(name) || i.value().isEmpty() || i.value().size() > 4096) return false;
        for (const auto ch : i.value()) if (uchar(ch) < 32 || uchar(ch) == 127) return false;
        if (upbit ? (name != "authorization" && name != "accept") : name != "x-mbx-apikey") return false;
        if (upbit && name == "authorization" && !i.value().startsWith("Bearer ")) return false;
        if (upbit && name == "accept" && i.value() != "application/json") return false;
        if (!upbit && !QRegularExpression("\\A[A-Za-z0-9]{1,512}\\z").match(QString::fromLatin1(i.value())).hasMatch()) return false;
        headers.insert(name);
    }
    if (!headers.contains(upbit ? "authorization" : "x-mbx-apikey")) return false;
    QMap<QString, QString> fields; QStringList uuids;
    for (const auto& [key, value] : QUrlQuery(u).queryItems(QUrl::FullyDecoded)) {
        if (value.isEmpty() || value.size() > 128) return false;
        if (upbit && key == "uuids[]") { if (uuids.contains(value)) return false; uuids.append(value); continue; }
        if (fields.contains(key)) return false;
        fields.insert(key, value);
    }
    const auto integer = [&](const QString& key, qint64 low, qint64 high) {
        const auto s = fields.value(key); bool ok = false; const auto n = s.toLongLong(&ok);
        return ok && QString::number(n) == s && n >= low && n <= high;
    };
    if (upbit) {
        if (uuids.isEmpty()) {
            if (fields.keys() != QStringList{"limit", "order_by", "page"} || fields.value("limit") != "100" ||
                fields.value("order_by") != "desc" || !integer("page", 1, 10000)) return false;
        } else {
            static const QRegularExpression uuid("\\A[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\z");
            if (uuids.size() > 100 || fields.keys() != QStringList{"limit"} || fields.value("limit") != "100") return false;
            for (const auto& id : uuids) if (!uuid.match(id).hasMatch()) return false;
        }
        return true;
    }
    if (!uuids.isEmpty()) return false;
    constexpr qint64 LastTime = 253402300799999LL, Day = 86400000;
    QSet<QString> expected{"startTime", "endTime", "recvWindow", "timestamp", "signature"};
    if (source == FlowSource::BinanceTransfers) expected.unite({"type", "current", "size"});
    else expected.unite({"offset", "limit"});
    if (QSet<QString>(fields.keyBegin(), fields.keyEnd()) != expected) return false;
    if (!integer("timestamp", 1, LastTime) || !integer("recvWindow", 1, 60000) || !integer("startTime", 1, LastTime) ||
        !integer("endTime", 1, LastTime) || !QRegularExpression("\\A[a-f0-9]{64}\\z").match(fields.value("signature")).hasMatch()) return false;
    const auto start = fields.value("startTime").toLongLong(), end = fields.value("endTime").toLongLong();
    if (end < start || end > fields.value("timestamp").toLongLong()) return false;
    if (source == FlowSource::BinanceTransfers) {
        const auto types = flowSpotTransferTypes() + flowUsdmTransferTypes();
        if (!types.contains(fields.value("type")) || !integer("current", 1, 100000) || fields.value("size") != "100" ||
            end - start >= 7 * Day) return false;
    } else if (!integer("offset", 0, 1000000) || fields.value("limit") != "1000" || end - start >= 30 * Day) return false;
    return true;
}
} // namespace keefi
