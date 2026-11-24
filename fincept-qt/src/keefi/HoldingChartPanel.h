#pragma once
#include "keefi/HoldingMarketResolver.h"
#include <QHash>
#include <QPointer>
#include <QWidget>
class QTreeWidget;
class QTreeWidgetItem;
class QComboBox;
class QLabel;
class QHBoxLayout;
class QMenu;
namespace keefi {
class InfoButton;
// Transient presentation only. The resolver owns identity and market policy.
class HoldingChartPanel final : public QWidget {
    Q_OBJECT
public:
    explicit HoldingChartPanel(QWidget* parent=nullptr);
    ~HoldingChartPanel() override;
    void setRows(const QList<HoldingChartRow>&);
    void setWatchlistContext(WatchlistWorkspaceList*, const QString& listId);
    void setCoverageReason(const QString&);
    QWidget* informationButton();
    void setQuery(const QString&);
    void setChartIdentity(const QString&);
    void clearPrivate();
    QTreeWidget* resultTree() const { return tree_; }
    void activateResult(QTreeWidgetItem*);
    bool hasVisibleResults() const;
signals:
    void chartActivated(QString assetKey,QString marketKey);
    void marketSelected(QString assetKey,QString marketKey);
private:
    void selectRow(QTreeWidgetItem*);
    void updateDetails();
    void refreshQuotes();
    void toggleFavorite(QTreeWidgetItem*);
    void openContextMenu(const QPoint&);
    void selectMarket(const QString&);
    void syncInformation();
    QList<HoldingChartRow> rows_;
    QHash<QString,QString> defaultMarkets_; // Explanation only; never selects or persists a market.
    QTreeWidget* tree_=nullptr;
    QComboBox* markets_=nullptr;
    QLabel *empty_=nullptr,*detail_=nullptr;
    QPointer<InfoButton> information_;
    QHBoxLayout* informationRow_=nullptr;
    QPointer<QMenu> contextMenu_;
    QPointer<WatchlistWorkspaceList> watchlists_;
    QMetaObject::Connection quoteConnection_,modelDestroyedConnection_;
    QString listId_;
    QString coverageReason_; // Private transient explanation; cleared with the holding rows.
    QString selectedAsset_,query_,chartIdentity_,displayedMarket_;
};
}
