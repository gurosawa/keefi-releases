#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace keefi {
// Which chart intervals keep a toolbar button. A public display choice only:
// no identity, price, account or alert state. Stored apart from the study
// settings (chart/studies/v1) so earlier builds read their own key unchanged.
class ChartTimeframePins final {
  public:
    static constexpr const char* SettingsKey = "chart/timeframes/v1";
    static constexpr const char* Format = "keefi-chart-timeframes/1";
    static constexpr int MaxStateBytes = 1024;
    // 1분, 5분, 15분, 1시간, 4시간, 1일 in toolbar order.
    static const QList<qint64>& supported();
    static bool isSupported(qint64 intervalMs);

    ChartTimeframePins(); // Every supported interval pinned: the toolbar before pins existed.
    QList<qint64> pinned() const; // Toolbar order.
    bool isPinned(qint64 intervalMs) const;
    bool allPinned() const;
    // Refuses an unsupported interval and unpinning the only pinned interval.
    bool setPinned(qint64 intervalMs, bool pinned);
    QByteArray publicState() const;
    // Exactly {"format","pinned"}: supported whole-number intervals, no
    // duplicates, at least one. A refused state leaves the current one.
    bool restorePublicState(const QByteArray&, QString* reason = nullptr);

  private:
    QList<qint64> pinned_;
};
} // namespace keefi
