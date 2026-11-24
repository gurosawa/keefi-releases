#include "keefi/PortfolioAssetModel.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/LocalWindowHelpers_p.h"
#include "keefi/PlainToolTip.h"

#include <QDateTime>
#include <array>
#include <optional>
#include <variant>

namespace keefi {
namespace {
using local_window_detail::displayChoice;
const QString dash = QStringLiteral("—");
using Payload = std::variant<std::monostate, PortfolioGroupView, PortfolioHoldingView, PortfolioMissingView>;

class AssetItem final : public QStandardItem {
  public:
    QString key;
    int rank = 0;
    Payload value;
    unsigned changedColumns = 0;
    mutable std::array<std::optional<QString>, 6> text;
    bool operator<(const QStandardItem& other) const override {
        return rank < static_cast<const AssetItem&>(other).rank;
    }
};

QString storageText(const PortfolioGroupView& g) {
    return g.storages.size() == 1 ? g.storages.first()
        : QObject::tr("보관처 %1곳 · %2").arg(g.storages.size()).arg(g.storages.join(", "));
}

QString cellText(const PortfolioGroupView& g, int column, const PortfolioMoneyDisplay& money,
                 const QString& total, bool hideUnvalued) {
    switch (column) {
    case 0: return portfolioGroupLabel(g);
    case 1: return storageText(g);
    case 2: {
        const auto quantity = portfolioQuantity(g);
        if (!quantity.isEmpty()) return groupedAmount(quantity);
        if (g.holdings.size() == 1 && !g.holdings.first().quantity.isEmpty()) return portfolioHoldingQuantity(g.holdings.first());
        return QObject::tr("확인 필요");
    }
    case 3: {
        if (g.excluded) return QObject::tr("평가 대상 제외");
        for (const auto& h : g.holdings) if (h.state == PortfolioValueState::Fresh) {
            if (g.freshTotal.isEmpty()) return QObject::tr("자료 부족");
            return money.amount(g.freshTotal) + ((g.unvalued && !hideUnvalued) || g.references ? QObject::tr(" · 일부 제외") : QString{});
        }
        return g.unvalued ? QObject::tr("미평가") : g.references ? QObject::tr("참고 평가") : QObject::tr("합계 제외");
    }
    case 4: return g.presence == AssetPresence::NeedsCheck ? QObject::tr("수량 확인 필요 포함") : QString{};
    case 5: {
        const auto value = portfolioAllocationValue(g);
        return decimal::positive(value) ? portfolioShareText(value, total) : dash;
    }
    }
    return {};
}

QString cellText(const PortfolioHoldingView& h, int column, const PortfolioMoneyDisplay& money, const QString& total) {
    switch (column) {
    case 0: return portfolioAssetLabel(h.asset, h.displaySymbol);
    case 1: return h.accountAlias + " · " + displayChoice(h.market);
    case 2: return h.quantity.isEmpty() ? QObject::tr("확인 필요") : portfolioHoldingQuantity(h);
    case 3:
        if (h.excludedByUser) return QObject::tr("평가 대상 제외");
        switch (h.state) {
        case PortfolioValueState::Fresh: return money.amount(h.value);
        case PortfolioValueState::Reference: return QObject::tr("참고 %1").arg(money.amount(h.referenceValue));
        case PortfolioValueState::NotCounted: return QObject::tr("합계 미포함");
        case PortfolioValueState::Unvalued: return QObject::tr("미평가");
        }
        break;
    case 4: {
        QString stamp = h.balanceTimeMs <= 0 ? QObject::tr("시각 미확인") :
            QDateTime::fromMSecsSinceEpoch(h.balanceTimeMs).toString("yyyy-MM-dd HH:mm:ss");
        if (!h.reason.isEmpty()) stamp += " · " + h.reason;
        if (h.presence == AssetPresence::NeedsCheck) stamp += QObject::tr(" · 수량 확인 필요");
        return stamp;
    }
    case 5: return h.state == PortfolioValueState::Fresh && h.kind != AssetComponentKind::Liability && decimal::positive(h.value)
        ? portfolioShareText(h.value, total) : dash;
    }
    return {};
}

QString missingText(const PortfolioMissingView& m, int column) {
    switch (column) {
    case 0: return m.active ? QObject::tr("조회 중") : QObject::tr("미조회");
    case 1: return m.alias + " · " + displayChoice(m.market);
    case 2: return QObject::tr("미확인");
    case 3: return dash;
    case 4: return m.stage.isEmpty() ? QObject::tr("조회 대기") : m.stage;
    default: return {};
    }
}

// Match the original ordered rows by stable key. Quotes usually preserve order.
// Qt's item sorting keeps persistent model indexes attached to their identities.
class AssetRows {
  public:
    AssetRows(QStandardItem* parent, unsigned changedColumns) : parent_(parent), changedColumns_(changedColumns) {}
    AssetItem* put(const QString& key, Payload value) {
        auto* row = static_cast<AssetItem*>(parent_->child(count_));
        if (unmatched_ || !row || row->key != key) {
            if (!unmatched_) {
                unmatched_.emplace();
                for (int i = count_; i < parent_->rowCount(); ++i) {
                    auto* old = static_cast<AssetItem*>(parent_->child(i));
                    (*unmatched_)[old->key].append(old);
                }
            }
            auto it = unmatched_->find(key);
            if (it == unmatched_->end()) {
                row = new AssetItem;
                row->key = key;
                added_.append(row);
            } else {
                row = it->takeFirst();
                if (it->isEmpty()) unmatched_->erase(it);
            }
        }
        row->rank = count_++;
        if (row->value != value) {
            row->value = std::move(value);
            row->changedColumns = 0x3f;
            row->text = {};
        } else {
            row->changedColumns = changedColumns_;
            for (int col = 0; col < 6; ++col) if (changedColumns_ & (1u << col)) row->text[col].reset();
        }
        return row;
    }
    void finish() {
        if (!unmatched_) {
            if (parent_->rowCount() > count_) parent_->removeRows(count_, parent_->rowCount() - count_);
            return;
        }
        for (const auto& unused : std::as_const(*unmatched_))
            for (auto* row : unused) parent_->removeRow(row->row());
        if (!added_.isEmpty()) parent_->appendRows(added_);
        for (int i = 0; i < parent_->rowCount(); ++i) {
            if (static_cast<AssetItem*>(parent_->child(i))->rank == i) continue;
            parent_->sortChildren(0, Qt::AscendingOrder);
            break;
        }
    }
  private:
    QStandardItem* parent_;
    std::optional<QHash<QString, QList<AssetItem*>>> unmatched_;
    QList<QStandardItem*> added_;
    int count_ = 0;
    unsigned changedColumns_;
};
} // namespace

PortfolioAssetModel::PortfolioAssetModel(QObject* parent) : QStandardItemModel(parent) {
    setHorizontalHeaderLabels({tr("자산"), tr("업체 · 시장"), tr("보유 수량"), tr("평가액 KRW"), tr("기준 시각 · 상태"), tr("전체 비중")});
    for (int column : {2,3,5}) setHeaderData(column, Qt::Horizontal, int(Qt::AlignRight | Qt::AlignVCenter), Qt::TextAlignmentRole);
}

void PortfolioAssetModel::setSnapshot(const QList<PortfolioGroupView>& groups, const QList<PortfolioMissingView>& missing,
                                       const PortfolioMoneyDisplay& money, const QString& allocationTotal, bool hideUnvalued) {
    const unsigned valueColumn = (money != money_ || hideUnvalued != hideUnvalued_) ? (1u << 3) : 0;
    const unsigned shareColumn = allocationTotal != allocationTotal_ ? (1u << 5) : 0;
    money_ = money;
    allocationTotal_ = allocationTotal;
    hideUnvalued_ = hideUnvalued;
    AssetRows roots(invisibleRootItem(), valueColumn | shareColumn);
    for (const auto& group : groups) {
        auto* root = roots.put("group:" + group.identity, group);
        if (root->columnCount() != 6) root->setColumnCount(6);
        AssetRows children(root, valueColumn | shareColumn);
        for (const auto& holding : group.holdings) children.put(portfolioHoldingKey(holding), holding);
        children.finish();
    }
    if (!missing.isEmpty()) {
        auto* root = roots.put(QStringLiteral("missing"), {});
        if (root->columnCount() != 6) root->setColumnCount(6);
        AssetRows children(root, valueColumn | shareColumn);
        for (const auto& item : missing) children.put(item.connectionId + ":unread", item);
        children.finish();
    }
    roots.finish();
    // All values and the allocation denominator have been published before a
    // consumer can read the new totals. Display formatting stays demand-driven.
    const auto notifyRows = [this](QStandardItem* parent) {
        // Coalesce neighbouring rows with the same changed columns. A global
        // allocation change affects all shares even when a holding is unchanged.
        const auto from = parent->index();
        int start = 0;
        while (start < parent->rowCount()) {
            const auto mask = static_cast<AssetItem*>(parent->child(start))->changedColumns;
            int end = start + 1;
            while (end < parent->rowCount() && static_cast<AssetItem*>(parent->child(end))->changedColumns == mask) ++end;
            if (mask) {
                int firstCol = 0, lastCol = 5;
                while (!(mask & (1u << firstCol))) ++firstCol;
                while (!(mask & (1u << lastCol))) --lastCol;
                emit dataChanged(index(start,firstCol,from), index(end-1,lastCol,from));
            }
            start = end;
        }
    };
    notifyRows(invisibleRootItem());
    for (int i = 0; i < rowCount(); ++i) notifyRows(item(i));
}
void PortfolioAssetModel::clearPrivate() {
    if (rowCount()) removeRows(0, rowCount());
    groupEvidence_.clear();
    holdingEvidence_.clear();
    money_ = {};
    allocationTotal_.clear();
}
void PortfolioAssetModel::setEvidence(QHash<QString, QList<int>> groups, QHash<QString, QList<int>> holdings) {
    if (groups == groupEvidence_ && holdings == holdingEvidence_) return;
    groupEvidence_.swap(groups);
    holdingEvidence_.swap(holdings);
    const auto notify = [this](const QModelIndex& parent, const auto& previous, const auto& current) {
        int first = -1;
        for (int row = 0; row <= rowCount(parent); ++row) {
            bool changed = false;
            if (row < rowCount(parent)) {
                const auto idx = index(row, 0, parent);
                const auto key = parent.isValid() ? data(idx, RowKeyRole).toString() : data(idx, Qt::UserRole + 1).toString();
                changed = previous.value(key) != current.value(key);
            }
            if (changed && first < 0) first = row;
            if (!changed && first >= 0) {
                emit dataChanged(index(first,3,parent), index(row-1,3,parent), {EvidenceNumbersRole});
                first = -1;
            }
        }
    };
    notify({}, groups, groupEvidence_);
    for (int i = 0; i < rowCount(); ++i) notify(index(i,0), holdings, holdingEvidence_);
}
Qt::ItemFlags PortfolioAssetModel::flags(const QModelIndex& idx) const {
    return idx.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
}

QVariant PortfolioAssetModel::data(const QModelIndex& idx, int role) const {
    if (!idx.isValid() || idx.column() >= 6) return {};
    const auto* item = static_cast<const AssetItem*>(itemFromIndex(idx.siblingAtColumn(0)));
    if (!item) return {};
    const auto* g = std::get_if<PortfolioGroupView>(&item->value);
    const auto* h = std::get_if<PortfolioHoldingView>(&item->value);
    const auto* m = std::get_if<PortfolioMissingView>(&item->value);
    const auto cell = [&](int column) -> const QString& {
        auto& text = item->text[column];
        if (!text) {
            if (g) text = cellText(*g, column, money_, allocationTotal_, hideUnvalued_);
            else if (h) text = cellText(*h, column, money_, allocationTotal_);
            else if (m) text = missingText(*m, column);
            else text = column == 0 ? tr("자산 조회 대기·확인 필요") : QString{};
        }
        return *text;
    };
    if (role == StorageLineRole) return g && (g->storages.size() != 1 || !g->storages.first().isEmpty());
    const int col = idx.column();
    if (role == Qt::DisplayRole || role == Qt::EditRole) return cell(col);
    if (col == 3 && role == EvidenceNumbersRole) return QVariant::fromValue(g ? groupEvidence_.value(g->identity) : holdingEvidence_.value(item->key));
    if (col == 3 && (role == Qt::ToolTipRole || role == Qt::AccessibleDescriptionRole)) return cell(3);
    if (role == Qt::TextAlignmentRole && (g || h) && (col == 2 || col == 3 || col == 5)) return int(Qt::AlignRight | Qt::AlignVCenter);
    if (col == 1 && role == Qt::ToolTipRole && g) return plainToolTip(cell(1));
    if (col != 0) return {};
    switch (role) {
    case RowKeyRole: return item->key;
    case PresenceRole: return int(g ? g->presence : h ? h->presence : AssetPresence::NeedsCheck);
    case ValueRole: return h ? QVariant(int(h->state)) : QVariant{};
    case Qt::UserRole:
        if (g && g->storages.size() == 1 && !g->holdings.isEmpty()) return g->holdings.first().connectionId;
        if (h) return h->connectionId;
        if (m) return m->connectionId;
        return {};
    case Qt::UserRole + 1:
        if (g) return g->identity;
        return !h && !m ? QVariant(QStringLiteral("missing")) : QVariant{};
    case AvatarRole: return g ? QVariant(portfolioAssetLabel(g->asset, g->displaySymbol)) : QVariant{};
    case Qt::ToolTipRole:
        if (g) return plainToolTip(g->displayName + "\n" + g->asset);
        if (h) return plainToolTip(h->displayName + "\n" + h->asset);
        return {};
    case Qt::AccessibleTextRole:
        if (g) return cell(0) + ", " + cell(1);
        if (h) return cell(1) + ", " + cell(0);
        return {};
    case SearchRole:
        if (g) return g->asset + " " + g->displaySymbol + " " + g->displayName + " " + g->storages.join(" ");
        if (h) return h->asset + " " + h->displaySymbol + " " + h->displayName + " " + h->accountAlias + " " + h->provider + " " + displayChoice(h->market);
        if (m) return m->alias + " 확인 필요 미조회";
        return {};
    default: return {};
    }
}
void PortfolioAssetModel::multiData(const QModelIndex& idx, QModelRoleDataSpan roles) const {
    for (auto& requested : roles) requested.setData(data(idx, requested.role()));
}
} // namespace keefi
