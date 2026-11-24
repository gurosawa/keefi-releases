#pragma once
#include "keefi/TradingTypes.h"
#include <QDate>
#include <QList>
#include <QString>

namespace keefi {
struct UsdKrwPosting;

// KRW per USD history for display-only conversions (시장 비교 탭).
// Daily: ECB reference rates (KRW per EUR / USD per EUR).
// Intraday: Hana Bank postings recorded by the collector.
struct FxRate {
    QDate date;        // ECB reference date, or the KST date of a Hana posting.
    QString krwPerUsd; // Decimal text.
    double value = 0;
    qint64 postedAtMs = 0, knownAtMs = 0, receivedAtMs = 0;
    QString source;    // "ecb" or "hana"
};

struct FxChoice {
    bool available = false, approximate = false, previous = false;
    double krwPerUsd = 0;
    QString source, label; // 하나은행 고시 / ECB 일별 / ECB 일별(근사) / 환산 불가
    qint64 postedAtMs = 0, knownAtMs = 0; // Selected posting's availability, distinct from its posting time.
    QDate date;
};

class FxHistory final {
  public:
    static constexpr qsizetype MaximumEcbBytes = 16 * 1024 * 1024;
    static constexpr qint64 HanaMaximumAgeMs = 24LL * 3600000;
    static constexpr qint64 HanaPreviousAfterMs = 6LL * 3600000;

    static RequestSpec ecbHistoryRequest(); // Full history XML (gzip transfer).
    static bool isRequest(const RequestSpec&);
    // Ascending by date. Days without both KRW and USD are skipped.
    static QList<FxRate> parseEcb(const QByteArray& xml, qint64 receivedAtMs, QString* reason = nullptr);
    // ECB publishes "around 16:00 CET"; KEEFI treats a date as known at 17:00 Europe/Berlin.
    static qint64 ecbKnownAtMs(const QDate&);
    static FxRate fromHana(const UsdKrwPosting&);
    // Both lists ascending by knownAtMs. barCloseMs is the bar's last millisecond.
    // Daily bars use ECB. Shorter bars use Hana, then ECB as an approximation.
    // Nothing known after the bar ended is ever used.
    static FxChoice choose(qint64 barCloseMs, qint64 intervalMs, const QList<FxRate>& ecb, const QList<FxRate>& hana);
};
} // namespace keefi
