#include "keefi/AssetListFilter.h"
#include "keefi/LocalComboBox.h"
#include "keefi/SegmentedComboBox.h"
#include "keefi/TradingTypes.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QTableWidget>
#include <algorithm>

namespace keefi {
namespace {
constexpr int PresenceRole = Qt::UserRole + 100;
constexpr int SearchRole = PresenceRole + 1;
}

AssetPresence assetPresence(const QStringList& required, const QStringList& optional) {
    bool unknown = required.isEmpty();
    bool held = false;
    const auto inspect = [&](const QString& value) {
        const auto magnitude = value.startsWith('-') ? value.mid(1) : value;
        if (!decimal::valid(magnitude)) unknown = true;
        else if (decimal::normalize(magnitude) != "0") held = true;
    };
    for (const auto& value : required) inspect(value);
    for (const auto& value : optional) if (!value.isEmpty()) inspect(value);
    return held ? AssetPresence::Held : unknown ? AssetPresence::NeedsCheck : AssetPresence::Zero;
}

QList<int> assetDisplayOrder(const QList<AssetPresence>& presence) {
    QList<int> result;
    result.reserve(presence.size());
    for (int i = 0; i < presence.size(); ++i) result.append(i);
    std::stable_sort(result.begin(), result.end(), [&](int a, int b) { return presence[a] < presence[b]; });
    return result;
}

AssetListFilter::AssetListFilter(const QString& name, QWidget* parent) : QWidget(parent) {
    setObjectName(name);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    scope_ = new SegmentedComboBox(this);
    scope_->setObjectName(name + "Scope");
    scope_->addItem(tr("보유 자산"), true);
    scope_->addItem(tr("전체 자산"), false);
    // Use the final themed font metrics, including room for the selection mark.
    // A construction-time minimum width can leave the segments clipped later.
    scope_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    search_ = new QLineEdit(this);
    search_->setObjectName(name + "Search");
    search_->setPlaceholderText(tr("자산 검색 · 예: PUMP"));
    search_->setClearButtonEnabled(true);
    search_->setMaxLength(160);
    status_ = new QLabel(this);
    status_->setObjectName(name + "Status");
    status_->setProperty("type", "muted");
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    layout->addWidget(scope_);
    layout->addWidget(search_, 1);
    layout->addWidget(status_, 1);
    connect(scope_, &QComboBox::currentIndexChanged, this, &AssetListFilter::changed);
    connect(search_, &QLineEdit::textChanged, this, &AssetListFilter::changed);
    connect(this, &AssetListFilter::changed, this, [this] { if (target_) apply(complete_); });
}

AssetListFilter::Criteria AssetListFilter::criteria() const {
    return {search_->text().trimmed(), scope_->currentData().toBool()};
}
bool AssetListFilter::Criteria::matches(const QString& searchText, AssetPresence presence) const {
    return (!hideZero || presence != AssetPresence::Zero) &&
        (search.isEmpty() || searchText.contains(search, Qt::CaseInsensitive));
}
bool AssetListFilter::matches(const QString& searchText, AssetPresence presence) const {
    return criteria().matches(searchText, presence);
}

void AssetListFilter::setCounts(int visible, int held, int unknown, bool complete) {
    if (!search_->text().trimmed().isEmpty() && visible == 0)
        status_->setText(tr("검색 결과가 없습니다. 전체 자산으로도 확인하세요.") +
            (unknown > 0 ? tr(" · 수량 확인 필요 %1개").arg(unknown) : QString{}));
    else if (complete && held == 0 && unknown == 0)
        status_->setText(tr("확인된 보유 자산이 없습니다. 전체 자산으로 전환할 수 있습니다."));
    else
        status_->setText(tr("표시 %1개 · 보유 %2개 · 확인 필요 %3개%4").arg(visible).arg(held).arg(unknown)
            .arg(complete ? QString{} : tr(" · 미조회·실패 여부 확인")));
}

void AssetListFilter::annotate(QTableWidget* table, int row, AssetPresence presence, const QString& searchText) {
    auto* item = table->item(row, 0);
    if (!item) return;
    if (item->data(PresenceRole) != int(presence)) item->setData(PresenceRole, int(presence));
    if (item->data(SearchRole) != searchText) item->setData(SearchRole, searchText);
    const auto tip = presence == AssetPresence::NeedsCheck ? tr("수량 확인 필요 · 잔고 0으로 간주하지 않습니다.") : QString{};
    if (item->toolTip() != tip) item->setToolTip(tip);
}

void AssetListFilter::setTarget(QTableWidget* target) {
    target_ = target;
    if (target_) apply(complete_);
}

void AssetListFilter::apply(bool complete) {
    complete_ = complete;
    if (!target_) return;
    int visible = 0, held = 0, unknown = 0;
    for (int row = 0; row < target_->rowCount(); ++row) {
        const auto* item = target_->item(row, 0);
        const auto presence = item && item->data(PresenceRole).isValid()
            ? AssetPresence(item->data(PresenceRole).toInt()) : AssetPresence::NeedsCheck;
        held += presence == AssetPresence::Held;
        unknown += presence == AssetPresence::NeedsCheck;
        const bool show = matches(item ? item->data(SearchRole).toString() : QString{}, presence);
        if (target_->isRowHidden(row) == show) target_->setRowHidden(row, !show);
        visible += show;
    }
    setCounts(visible, held, unknown, complete);
}
} // namespace keefi
