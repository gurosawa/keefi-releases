#pragma once
#include "keefi/CoinFlowStudies.h"
#include "keefi/MovementDirection.h"
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QWidget>

class QTimer;
class QVariantAnimation;

namespace keefi {
class CoinFlowSource;

// Recent public trades of the chart's own venue and same-coin USD-M perpetual
// liquidations, newest first, in each trade's own quote currency. One painter,
// no item view and no keyboard focus: a refresh never moves focus or recreates
// child widgets. Reads CoinFlowSource::snapshot() only while it is shown.
class CoinTapeView final : public QWidget {
    Q_OBJECT
  public:
    struct Row {
        QString key, venue, price, amount;
        qint64 atMs = 0;
        MovementDirection side = MovementDirection::Unknown;
        bool liquidation = false, large = false;
    };
    static constexpr int MaxRows = 120;
    static constexpr int FlashMs = 520;
    static constexpr int RowHeight = 40;
    // Newest trades are gathered into one repaint at most this often.
    static constexpr int RefreshMs = 150;

    explicit CoinTapeView(QWidget* parent = nullptr);
    void setSource(CoinFlowSource*);
    // Full chart identity. Anything but a coin chart shows the empty state.
    void setIdentity(const QString& fullIdentity);
    // Shows one source snapshot; a snapshot of another identity shows nothing.
    // The connected source's own snapshots arrive here on each paced refresh.
    void showSnapshot(const CoinFlowSnapshot&);
    QString identity() const { return identity_; }
    QList<Row> rows() const { return rows_; }
    QString emptyText() const;
    bool flashing() const;
    QStringList flashingKeys() const { return flashKeys_; }
    static QString venueName(CoinFlowVenue);
    // Exact price × quantity, rounded for display only; unit is the quote currency.
    static QString amountText(const QString& exactNotional, const QString& quoteAsset);

  signals:
    // Emitted on every show and hide, including those of an ancestor.
    void shownChanged(bool shown);

  protected:
    void paintEvent(QPaintEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;

  private:
    void scheduleRefresh();
    void refresh();
    QRect rowRect(int index) const;
    QPointer<CoinFlowSource> source_;
    QString identity_;
    QList<Row> rows_;
    QHash<QString, Row> cache_;
    QStringList flashKeys_;
    QTimer* refreshTimer_ = nullptr;
    QVariantAnimation* flash_ = nullptr;
    qreal flashProgress_ = 1.0;
    qint64 lastRefreshMs_ = 0;
    // The large-trade marks change only when a whole observed minute closes,
    // so the rule runs again only then (or for a new window).
    QString largeKey_;
    QSet<QString> largeIds_;
};
} // namespace keefi
