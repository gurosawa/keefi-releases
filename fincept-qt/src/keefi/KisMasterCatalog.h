#pragma once
#include "keefi/TradingTypes.h"

namespace keefi {
// Public KIS master data only. Decodes in memory; never extracts a ZIP to disk.
class KisMasterCatalog {
  public:
    static QString sourceUrl(const QString& market);
    static QList<MarketInstrument> parseZip(const QByteArray& bytes, const QString& market,
                                           QString* error = nullptr);
};
}
