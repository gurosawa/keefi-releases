#pragma once

#include "keefi/AccountModel.h"
#include "keefi/PortfolioController.h"
#include <QAbstractListModel>
#include <QStyledItemDelegate>

namespace keefi {

QString connectionMarketName(const QString& market);
QString connectionEnvironmentName(const QString& environment);
QString connectionStateName(const PortfolioConnection& connection);
bool connectionNeedsAttention(const PortfolioConnection& connection);
QString connectionLastSuccess(qint64 receivedAtMs);

struct WalletReadResult {
    QString state, detail;
    bool hasAssets = false;
};

std::optional<WalletReadResult> phantomWalletReadResult(const PortfolioConnection&, const PortfolioController&);

struct ConnectionGroup {
    QString id;
    QList<AccountRecord> accounts;
};

// Presentation only. Group by an existing shared credential reference, never by
// alias, provider alone, or credential values. Account IDs remain action targets.
class ConnectionListModel final : public QAbstractListModel {
  public:
    enum Role { ProviderRole = Qt::UserRole + 100, ContextRole, StatusLinesRole, ProblemLinesRole };
    explicit ConnectionListModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex&, int role = Qt::DisplayRole) const override;
    void refresh(const QList<AccountRecord>&, const QList<PortfolioConnection>&);
    ConnectionGroup groupAt(int row) const;
    int rowForAccount(const QString& id) const;

  private:
    QList<ConnectionGroup> groups_;
    QHash<QString, PortfolioConnection> states_;
};

class ConnectionRowDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override;
    void paint(QPainter*, const QStyleOptionViewItem&, const QModelIndex&) const override;
};

} // namespace keefi
