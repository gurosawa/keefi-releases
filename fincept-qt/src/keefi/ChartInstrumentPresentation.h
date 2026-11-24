#pragma once
#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/KeefiFonts.h"
#include <QObject>
#include <QHash>
#include <QPointer>

namespace keefi {
class PublicMarketCatalog;
class PublicEquityBoard;
class ChartInstrumentCatalog;

// Transient, public display text. Never a verification, persistence or share input.
struct ChartInstrumentLabel {
    QString name, code, market;
    QString text() const;
    QString titleHtml(const QString& suffix, FontRole role = FontRole::BodyStrong15) const;
};

// Reads the existing public caches only. It cannot request or promote an identity.
class ChartInstrumentPresentation final : public QObject {
    Q_OBJECT
  public:
    explicit ChartInstrumentPresentation(QObject* parent = nullptr) : QObject(parent) {}
    void setSources(PublicMarketCatalog*, PublicEquityBoard*, ChartInstrumentCatalog*);
    ChartInstrumentLabel label(const WatchlistIdentity&) const;
    // Same labels as label(), indexing each public catalogue once for a list.
    QHash<QString, ChartInstrumentLabel> labels(const QList<WatchlistIdentity>&) const;
    static ChartInstrumentLabel fallback(const WatchlistIdentity&);
  signals:
    void changed();
  private:
    QPointer<PublicMarketCatalog> markets_;
    QPointer<PublicEquityBoard> board_;
    QPointer<ChartInstrumentCatalog> instruments_;
    QList<QMetaObject::Connection> connections_;
};
}
