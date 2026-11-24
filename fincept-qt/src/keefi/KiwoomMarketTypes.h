#pragma once
// Display-only public market types; never an account/valuation input.
#include "keefi/AssetValuation.h"
#include "keefi/MovementDirection.h"
#include "keefi/WatchlistWorkspaceList.h"
#include <QByteArray>
#include <QDate>
#include <QList>
namespace keefi {
enum class KiwoomDisplayRequest { KrCatalog, KrMetadata, UsCatalog, UsMetadata, KrPrice, UsPrice, KrDaily };
struct KiwoomReadIntent {
    KiwoomDisplayRequest read=KiwoomDisplayRequest::KrMetadata;
    QString country,symbol,exchange,catalogMarket;
    QString baseDate; // KrDaily only: KST yyyyMMdd; the newest trading day to return.
    QByteArray cursor; // Bounded opaque response cursor, transient only, not UI/profile.
};
struct KiwoomVerifiedInstrument {
    WatchlistIdentity identity;
    QString exchange,currency; // Exact official route and separately proven KRW/USD.
    AssetEvidence metadataEvidence,currencyEvidence;
    QString metadataApiId,currencyApiId;
    bool verified=false,nxtEligible=false;
};
struct KiwoomMarketTarget {
    WatchlistIdentity identity;
    QString exchange; // Empty US resolves only when official metadata has exactly one match.
    int priority=3; // selected0 > held1 > visible watchlist2 > remaining watchlist3.
    bool privateOnly=false;
    bool operator==(const KiwoomMarketTarget&) const=default;
};
struct KiwoomMarketQuote {
    WatchlistIdentity identity;
    QString exchange,currency,price,rawSignedPrice;
    QString rawDate,rawTime,rawLocalTime;
    AssetEvidence evidence;
    bool realtime=false; // Confirmed live source state, never candle/alert eligibility.
    QString changeText; // Proven percent display only, no ▲/▼; empty means unknown.
    MovementDirection direction=MovementDirection::Unknown;
    AssetEvidence changeEvidence;
};
struct KiwoomMetadataPage {
    bool available=false,continued=false,rateLimited=false,authenticationFailed=false;
    QList<KiwoomVerifiedInstrument> instruments; // US metadata alone has empty currency/unverified until price proof joins.
    QByteArray nextCursor;
    QString reason; // Fixed public reason only; never return_msg/raw payload.
};
// ka10081 daily bars: exact decimal strings, oldest first. Display only.
struct KiwoomDailyRow { QDate date; QString open,high,low,close,volume; };
struct KiwoomDailyPage {
    bool available=false,rateLimited=false,authenticationFailed=false;
    QString reason;QList<KiwoomDailyRow> rows;
};
struct KiwoomDailySeries {
    WatchlistIdentity identity;QList<KiwoomDailyRow> rows;
    qint64 requestStartedAtMs=0,receivedAtMs=0;bool available=false,loading=false;QString reason;
};
struct KiwoomRestPrice {
    bool available=false,rateLimited=false,authenticationFailed=false;
    KiwoomMarketQuote quote;
    AssetEvidence currencyEvidence;
    QString currencyApiId,reason;
};
struct KiwoomControlAck {bool available=false,accepted=false,authenticationFailed=false;QString command;};
}
