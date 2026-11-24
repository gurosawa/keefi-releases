#pragma once

#include "keefi/PortfolioMoneyDisplay.h"
#include <QStandardItemModel>

namespace keefi {

// Each item holds the valued source row. Presentation strings are created only
// when a view, assistive client or detail panel asks for that cell.
class PortfolioAssetModel final : public QStandardItemModel {
  public:
    enum Role { PresenceRole = Qt::UserRole + 2, SearchRole, RowKeyRole, ValueRole, StorageLineRole };
    explicit PortfolioAssetModel(QObject* parent);
    void setSnapshot(const QList<PortfolioGroupView>& groups, const QList<PortfolioMissingView>& missing,
                     const PortfolioMoneyDisplay& money, const QString& allocationTotal, bool hideUnvalued);
    void setEvidence(QHash<QString, QList<int>> groups, QHash<QString, QList<int>> holdings);
    void clearPrivate();
    QVariant data(const QModelIndex&, int role = Qt::DisplayRole) const override;
    void multiData(const QModelIndex&, QModelRoleDataSpan) const override;
    Qt::ItemFlags flags(const QModelIndex&) const override;

  private:
    QHash<QString, QList<int>> groupEvidence_, holdingEvidence_;
    PortfolioMoneyDisplay money_;
    QString allocationTotal_;
    bool hideUnvalued_ = false;
};
} // namespace keefi
