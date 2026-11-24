#pragma once
#include "keefi/TradingTypes.h"
#include <QRegularExpression>
#include <QSet>
#include <QUrlQuery>

namespace keefi {
// Pure read authority, also linked by transport-only tests. No order, subscribe,
// redemption or transfer execution endpoint is allowed by this module.
enum class EarnSource {
    FlexiblePositions, LockedPositions, FlexibleSubscriptions, LockedSubscriptions,
    FlexibleRedemptions, LockedRedemptions, FlexibleRewards, LockedRewards
};
inline QList<EarnSource> earnSources() {
    return {EarnSource::FlexiblePositions, EarnSource::LockedPositions,
        EarnSource::FlexibleSubscriptions, EarnSource::LockedSubscriptions,
        EarnSource::FlexibleRedemptions, EarnSource::LockedRedemptions,
        EarnSource::FlexibleRewards, EarnSource::LockedRewards};
}
inline bool earnPositions(EarnSource s) {
    return s == EarnSource::FlexiblePositions || s == EarnSource::LockedPositions;
}
inline bool earnReward(EarnSource s) {
    return s == EarnSource::FlexibleRewards || s == EarnSource::LockedRewards;
}
inline QString earnPath(EarnSource s) {
    switch (s) {
    case EarnSource::FlexiblePositions: return "/sapi/v1/simple-earn/flexible/position";
    case EarnSource::LockedPositions: return "/sapi/v1/simple-earn/locked/position";
    case EarnSource::FlexibleSubscriptions: return "/sapi/v1/simple-earn/flexible/history/subscriptionRecord";
    case EarnSource::LockedSubscriptions: return "/sapi/v1/simple-earn/locked/history/subscriptionRecord";
    case EarnSource::FlexibleRedemptions: return "/sapi/v1/simple-earn/flexible/history/redemptionRecord";
    case EarnSource::LockedRedemptions: return "/sapi/v1/simple-earn/locked/history/redemptionRecord";
    case EarnSource::FlexibleRewards: return "/sapi/v1/simple-earn/flexible/history/rewardsRecord";
    case EarnSource::LockedRewards: return "/sapi/v1/simple-earn/locked/history/rewardsRecord";
    }
    return {};
}
inline QString earnSourceToken(EarnSource s) { return earnPath(s).mid(QString("/sapi/v1/simple-earn/").size()); }
inline bool isEarnRead(Operation op) { return op == Operation::EarnRead; }
inline bool allowedEarnRead(const RequestSpec& r) {
    if (!isEarnRead(r.operation) || !r.authenticated || r.method != "GET" || !r.body.isEmpty()) return false;
    const auto& u = r.url;
    if (!u.isValid() || u.scheme() != "https" || u.host() != "api.binance.com" ||
        u.port(443) != 443 || !u.userInfo().isEmpty() || u.hasFragment()) return false;
    bool found = false; EarnSource source = EarnSource::FlexiblePositions;
    for (const auto s : earnSources()) if (u.path(QUrl::FullyEncoded) == earnPath(s)) { source = s; found = true; break; }
    if (!found) return false;
    QSet<QByteArray> headers;
    for (auto i = r.headers.cbegin(); i != r.headers.cend(); ++i) {
        const auto name = i.key().toLower();
        if (headers.contains(name) || name != "x-mbx-apikey" || i.value().isEmpty() || i.value().size() > 512 ||
            !QRegularExpression("\\A[A-Za-z0-9]+\\z").match(QString::fromLatin1(i.value())).hasMatch()) return false;
        headers.insert(name);
    }
    if (!headers.contains("x-mbx-apikey")) return false;
    QSet<QString> expected{"timestamp", "recvWindow", "signature", "current", "size"};
    if (!earnPositions(source)) expected.unite({"startTime", "endTime"});
    if (source == EarnSource::FlexibleRewards) expected.insert("type");
    QMap<QString, QString> fields;
    for (const auto& pair : QUrlQuery(u).queryItems(QUrl::FullyDecoded)) {
        if (!expected.contains(pair.first) || fields.contains(pair.first) || pair.second.isEmpty() || pair.second.size() > 128) return false;
        fields.insert(pair.first, pair.second);
    }
    if (fields.size() != expected.size()) return false;
    const auto integer = [&](const QString& key, qint64 low, qint64 high) {
        const auto s = fields.value(key); bool ok = false; const auto n = s.toLongLong(&ok);
        return ok && QString::number(n) == s && n >= low && n <= high;
    };
    constexpr qint64 LastTime = 253402300799999LL;
    if (!integer("timestamp", 1, LastTime) || !integer("recvWindow", 1, 60000) ||
        !integer("current", 1, 100000) || !integer("size", 1, 100) ||
        !QRegularExpression("\\A[a-f0-9]{64}\\z").match(fields.value("signature")).hasMatch()) return false;
    if (source == EarnSource::FlexibleRewards && fields.value("type") != "ALL") return false;
    if (!earnPositions(source)) {
        if (!integer("startTime", 1, LastTime) || !integer("endTime", 1, LastTime)) return false;
        const auto start = fields.value("startTime").toLongLong(), end = fields.value("endTime").toLongLong();
        if (end < start || end - start >= 30LL * 86400000 || end > fields.value("timestamp").toLongLong()) return false;
    }
    return true;
}
} // namespace keefi
