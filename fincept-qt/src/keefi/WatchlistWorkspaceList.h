#pragma once
#include "keefi/MovementDirection.h"
#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QIODevice>
#include <QStringList>

namespace keefi {
struct WatchlistIdentity {
    QString market, source, code;
    QString key() const;
    bool operator==(const WatchlistIdentity&) const = default;
};
struct WatchlistSection {
    QString id, name;
    bool operator==(const WatchlistSection&) const = default;
};
struct WatchlistEntry {
    WatchlistIdentity identity;
    QString sectionId;
    bool operator==(const WatchlistEntry&) const = default;
};
struct WatchlistUserList {
    QString id, name, flag;
    QList<WatchlistSection> sections;
    QList<WatchlistEntry> entries;
    bool operator==(const WatchlistUserList&) const = default;
};
struct WatchlistBadge {
    QString text;
    MovementDirection direction = MovementDirection::Unknown;
    bool fixedRisk = false;
    bool operator==(const WatchlistBadge&) const = default;
};
enum class WatchlistQuoteState { Receiving, Disconnected, Unsupported, ConnectionRequired, Delayed };
struct WatchlistRowState {
    QString price, change, evidence;
    QStringList badges;
    qint64 completedAtMs = 0;
    bool privateData = false;
    QList<WatchlistBadge> badgePresentations;
    WatchlistQuoteState quoteState = WatchlistQuoteState::Receiving; // Transient source status; never persisted.
    QString quoteCurrency; // Original source currency for a numeric-only display price; never inferred from the symbol.
    bool operator==(const WatchlistRowState&) const = default;
};

struct PublicTick;
struct MarketInstrument;
// Display projection only. Original public observation times remain unchanged;
// this is not a quote for valuation, alerts, or persistent list state.
WatchlistRowState projectBinanceWatchlistQuote(const WatchlistIdentity&, const PublicTick&,
    qint64 asOfMs, bool disconnected = false, quint64 currentStreamEpoch = 0);
// Already admitted, still-visible public observations can remain delayed while
// metadata refreshes. This projection never authorizes a new subscription.
WatchlistRowState projectRetainedBinanceWatchlistQuote(const WatchlistIdentity&, const PublicTick&,
    const QList<MarketInstrument>& currentInstruments, qint64 asOfMs,
    bool disconnected = false, quint64 currentStreamEpoch = 0);

// Public list state only. Callers store publicState() in their profile settings.
// Holdings, verification registry and row observations never enter that state.
class WatchlistWorkspaceList final : public QObject {
    Q_OBJECT
  public:
    explicit WatchlistWorkspaceList(QObject* parent = nullptr);
    static bool validIdentity(const WatchlistIdentity&);
    static WatchlistIdentity identityFromKey(const QString&);
    void setVerifiedInstruments(const QList<WatchlistIdentity>&);
    // Public catalogue membership permits saving a stock, never broker reads.
    void setPublicStockInstruments(const QList<WatchlistIdentity>&);
    bool isCurrentlyVerified(const WatchlistIdentity&) const;
    QList<WatchlistUserList> userLists() const;
    WatchlistUserList userList(const QString& id) const;
    QString createList(const QString& name, const QString& flag = {});
    bool renameList(const QString& id, const QString& name);
    bool setFlag(const QString& id, const QString& flag);
    bool removeList(const QString& id);
    bool moveList(const QString& id, int index);
    QString addSection(const QString& listId, const QString& name);
    bool renameSection(const QString& listId, const QString& sectionId, const QString& name);
    bool removeSection(const QString& listId, const QString& sectionId);
    bool moveSection(const QString& listId, const QString& sectionId, int index);
    bool addIdentity(const QString& listId, const WatchlistIdentity&, const QString& sectionId = {});
    // Repair one explicitly chosen public entry without changing its section/order.
    // If the target already exists, keep that entry and remove only the old one.
    bool replaceIdentity(const QString& listId, const WatchlistIdentity& oldIdentity, const WatchlistIdentity& newIdentity);
    bool removeIdentity(const QString& listId, const WatchlistIdentity&);
    bool moveIdentity(const QString& listId, const WatchlistIdentity&, int index, const QString& sectionId = {});
    void useList(const QString& id);
    QStringList recentLists() const;
    void setHoldings(const QList<WatchlistIdentity>&);
    QList<WatchlistIdentity> holdings() const;
    void setRowState(const WatchlistIdentity&, const WatchlistRowState&);
    void setRowStates(const QHash<QString, WatchlistRowState>&);
    WatchlistRowState rowState(const WatchlistIdentity&) const;
    void clearPrivate();
    QByteArray publicState() const;
    bool restorePublicState(const QByteArray&, QString* error = nullptr);
    QByteArray exportCsv(const QString& listId) const;
    QString importCsv(const QByteArray&, QString* error = nullptr);
    // The caller supplies an opened finite file/device; the CSV format is unchanged.
    QString importCsv(QIODevice&, QString* error = nullptr);
    // Input is a read-only copy of the original explore/watchlist keys.
    // Unresolved symbols stay pending; repeated calls never duplicate a row.
    int migrateLegacy(const QMap<QString, QStringList>&);
    static QList<WatchlistIdentity> rankRealtime(const QList<WatchlistIdentity>& visible,
        const QList<WatchlistIdentity>& selected, const QList<WatchlistIdentity>& holdings,
        const QList<WatchlistIdentity>& rest, int cap);
  signals:
    void changed();
  private:
    QList<WatchlistUserList> lists_;
    QList<WatchlistIdentity> verified_, publicStocks_, holdings_;
    QHash<QString, WatchlistRowState> rows_;
    QStringList recent_, migrated_;
};
}
Q_DECLARE_METATYPE(keefi::WatchlistBadge)
