#include "keefi/ChartWorkspacePresentation_p.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/KeefiFonts.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/UsabilityPresentation.h"
#include <QLabel>
#include <QRegularExpression>
#include <QTreeWidget>
#include <cmath>

namespace keefi::chart_workspace_detail {
const QString HoldingsList = "@holdings";

QString splitStockChartMessage() {
    return ChartWorkspace::tr("분할 보기는 공개 현물부터 지원해요. 주식은 단일 차트에서 열어 주세요.");
}

bool isVisibleInstrumentRow(const QTreeWidgetItem* item) {
    if (!item || item->isHidden() || item->data(0, IdentityRole).toString().isEmpty()) return false;
    for (auto* parent = item->parent(); parent; parent = parent->parent())
        if (parent->isHidden() || !parent->isExpanded()) return false;
    return true;
}
// Each coin exchange has its own browser tab, so one coin is listed once per
// tab instead of once per exchange. Bithumb holdings stay with the KRW tab.
QString browserMarket(const WatchlistIdentity& identity) {
    if (identity.market == "stock") return identity.source.toLower();
    return identity.source == "binance" ? QStringLiteral("binance") : QStringLiteral("upbit");
}
bool coinBrowserMarket(const QString& market) { return market == "upbit" || market == "binance"; }
bool canSelectIdentity(const WatchlistWorkspaceList* lists, const WatchlistIdentity& identity) {
    if (!WatchlistWorkspaceList::validIdentity(identity)) return false;
    if (identity.market != "stock") return true;
    if (!lists) return false;
    if (lists->isCurrentlyVerified(identity)) return true;
    // A saved public row may explain the missing broker without becoming
    // verified metadata for drawing, alerts, shares or adding new stocks.
    for (const auto& list : lists->userLists()) for (const auto& entry : list.entries)
        if (entry.identity == identity) return true;
    return false;
}
bool positiveDisplayNumber(const QString& value) {
    static const QRegularExpression number("\\A[0-9]{1,80}(?:\\.[0-9]{1,80})?\\z");
    return number.match(value).hasMatch() && decimal::positive(value) && std::isfinite(value.toDouble());
}
QString studyPeriod(qint64 value) {
    if(value>=86400000&&value%86400000==0)return QObject::tr("%1일").arg(value/86400000);
    if(value>=3600000&&value%3600000==0)return QObject::tr("%1시간").arg(value/3600000);
    return QObject::tr("%1분").arg(value/60000);
}
void setSelectableLedgerText(QLabel* label,const QString& plain) {
    const auto rendered=KeefiFonts::mixedTextHtml(plain,FontRole::Body15);
    // A refresh of the same observation must leave a native selection intact.
    if(label->text()!=rendered)label->setText(rendered);
}
QPair<QString,QString> quotePriceParts(const QString& raw) {
    static const QRegularExpression price("\\A([0-9]+(?:\\.[0-9]+)?)(?: ([A-Z]{2,10}))?\\z");
    const auto match=price.match(raw);
    return match.hasMatch()&&positiveDisplayNumber(match.captured(1))
        ? qMakePair(priceAmount(match.captured(1)),match.captured(2)) : QPair<QString,QString>{};
}
static QString missingQuoteText(WatchlistQuoteState state) {
    switch (state) {
    case WatchlistQuoteState::Disconnected: return QObject::tr("끊김");
    case WatchlistQuoteState::Unsupported: return QObject::tr("지원 없음");
    case WatchlistQuoteState::ConnectionRequired: return QObject::tr("연결 필요");
    case WatchlistQuoteState::Receiving: return QStringLiteral("—");
    case WatchlistQuoteState::Delayed: return QObject::tr("지연");
    }
    return QStringLiteral("—");
}
QString receivedQuoteStatus(WatchlistQuoteState state) {
    if(state==WatchlistQuoteState::Delayed)return QObject::tr("지연");
    if(state==WatchlistQuoteState::Disconnected)return QObject::tr("끊김");
    return {};
}
RowQuoteDisplay rowQuoteDisplay(const WatchlistRowState& row) {
    const auto parts=quotePriceParts(row.price);
    RowQuoteDisplay display;
    if(parts.first.isEmpty())display.text=missingQuoteText(row.quoteState);
    else {
        display.text=parts.first;
        if(!parts.second.isEmpty())display.text+=" "+parts.second;
        display.status=receivedQuoteStatus(row.quoteState);
    }
    QStringList detail;
    if(!display.status.isEmpty())detail.append(display.status);
    if(!row.change.isEmpty())detail.append(row.change);
    if(!detail.isEmpty())display.text+="\n"+detail.join(" · ");
    return display;
}
QList<WatchlistBadge> rowBadges(const WatchlistRowState& state, bool verified, bool delayed, qint64 now) {
    QList<WatchlistBadge> result;
    if (state.completedAtMs > 0 && state.completedAtMs <= now) {
        result = state.badgePresentations;
        if (result.isEmpty()) for (const auto& text : state.badges) result.append({text});
    }
    if (!verified) result.prepend({usabilityText(UsabilityText::NeedsCheck)});
    if (delayed) result.prepend({QObject::tr("지연")});
    return result;
}

} // namespace keefi::chart_workspace_detail
