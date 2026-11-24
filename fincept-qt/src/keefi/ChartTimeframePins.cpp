#include "keefi/ChartTimeframePins.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>

namespace keefi {
const QList<qint64>& ChartTimeframePins::supported() {
    static const QList<qint64> values{60000, 300000, 900000, 3600000, 14400000, 86400000};
    return values;
}
bool ChartTimeframePins::isSupported(qint64 intervalMs) { return supported().contains(intervalMs); }

ChartTimeframePins::ChartTimeframePins() : pinned_(supported()) {}
QList<qint64> ChartTimeframePins::pinned() const { return pinned_; }
bool ChartTimeframePins::isPinned(qint64 intervalMs) const { return pinned_.contains(intervalMs); }
bool ChartTimeframePins::allPinned() const { return pinned_.size() == supported().size(); }

bool ChartTimeframePins::setPinned(qint64 intervalMs, bool pin) {
    if (!isSupported(intervalMs)) return false;
    if (pin == isPinned(intervalMs)) return true;
    if (!pin) {
        if (pinned_.size() <= 1) return false; // The toolbar always keeps one interval.
        pinned_.removeAll(intervalMs);
        return true;
    }
    QList<qint64> next;
    for (const auto value : supported())
        if (value == intervalMs || pinned_.contains(value)) next.append(value);
    pinned_ = next;
    return true;
}

QByteArray ChartTimeframePins::publicState() const {
    QJsonArray values;
    for (const auto value : pinned_) values.append(value);
    return QJsonDocument(QJsonObject{{"format", QString::fromLatin1(Format)}, {"pinned", values}})
        .toJson(QJsonDocument::Compact);
}

bool ChartTimeframePins::restorePublicState(const QByteArray& bytes, QString* reason) {
    const auto fail = [reason] {
        if (reason) *reason = QObject::tr("시간봉 즐겨찾기 형식이 올바르지 않습니다.");
        return false;
    };
    if (bytes.isEmpty() || bytes.size() > MaxStateBytes) return fail();
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return fail();
    const auto object = document.object();
    if (object.size() != 2 || object.value("format") != QJsonValue(QString::fromLatin1(Format)) ||
        !object.value("pinned").isArray())
        return fail();
    const auto values = object.value("pinned").toArray();
    if (values.isEmpty() || values.size() > supported().size()) return fail();
    QList<qint64> read;
    for (const auto& value : values) {
        // Whole numbers only: 60000.5, "60000" or true are not intervals.
        const qint64 interval = value.isDouble() ? value.toInteger(-1) : -1;
        if (!isSupported(interval) || read.contains(interval)) return fail();
        read.append(interval);
    }
    QList<qint64> next;
    for (const auto value : supported())
        if (read.contains(value)) next.append(value);
    pinned_ = next;
    return true;
}
} // namespace keefi
