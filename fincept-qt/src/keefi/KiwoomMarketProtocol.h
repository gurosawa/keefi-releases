#pragma once
#include "KiwoomMarketTypes.h"
#include "keefi/TradingTypes.h"
#include <optional>
namespace keefi {
// Pure display protocol seam. No auth, network, account, clock or settings reads.
class KiwoomMarketProtocol final {
  public:
    static constexpr qsizetype MaxFrameBytes=65536,MaxRestBytes=4*1024*1024;
    static RequestSpec request(const KiwoomReadIntent&); // Authenticated Reference prototype, no bearer retained.
    static bool isRequest(const RequestSpec&); // Exact seven API ids/path/body/header/cursor shapes only.
    static KiwoomMetadataPage parseMetadata(const KiwoomReadIntent&,const HttpResult&,qint64 originalReceiptMs);
    static KiwoomRestPrice parseRestPrice(const KiwoomReadIntent&,const HttpResult&,qint64 originalReceiptMs);
    static KiwoomDailyPage parseDaily(const KiwoomReadIntent&,const HttpResult&,qint64 originalReceiptMs); // ka10081, first page only.
    static QByteArray subscription(const QString& country,const QList<KiwoomMarketTarget>& resolved,bool remove=false);
    static KiwoomControlAck controlAck(const QByteArray&,const QString& expectedCommand);
    static std::optional<QByteArray> pingEcho(const QByteArray&); // Exact original bounded bytes, no reserialization.
    static QList<KiwoomMarketQuote> realtimeQuotes(const QByteArray&,const QString& activeCountry,
        const QList<KiwoomVerifiedInstrument>& currentProof,qint64 originalReceiptMs);
};
}
