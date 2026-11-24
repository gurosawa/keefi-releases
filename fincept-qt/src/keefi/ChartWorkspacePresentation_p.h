#pragma once

#include "keefi/WatchlistWorkspaceList.h"
#include <QPair>
#include <QString>

class QLabel;
class QTreeWidgetItem;

namespace keefi::chart_workspace_detail {
extern const QString HoldingsList;
inline constexpr int IdentityRole = Qt::UserRole;
inline constexpr int SectionRole = Qt::UserRole + 1;
inline constexpr int BrowserSearchRole = Qt::UserRole + 40;
inline constexpr int BrowserMemberRole = Qt::UserRole + 41;
inline constexpr int BrowserUnavailableReasonRole = Qt::UserRole + 42;
inline constexpr int BrowserQuoteStatusRole = Qt::UserRole + 43;

QString splitStockChartMessage();
bool isVisibleInstrumentRow(const QTreeWidgetItem* item);
QString browserMarket(const WatchlistIdentity& identity);
bool coinBrowserMarket(const QString& market);
bool canSelectIdentity(const WatchlistWorkspaceList* lists, const WatchlistIdentity& identity);
bool positiveDisplayNumber(const QString& value);
QString studyPeriod(qint64 value);
void setSelectableLedgerText(QLabel* label, const QString& plain);
QPair<QString, QString> quotePriceParts(const QString& raw);
QString receivedQuoteStatus(WatchlistQuoteState state);
struct RowQuoteDisplay { QString text, status; };
RowQuoteDisplay rowQuoteDisplay(const WatchlistRowState& row);
QList<WatchlistBadge> rowBadges(const WatchlistRowState& state, bool verified, bool delayed, qint64 now);
} // namespace keefi::chart_workspace_detail
