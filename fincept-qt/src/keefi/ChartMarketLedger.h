#pragma once
#include <QList>
#include <QPolygonF>
#include <QString>
#include <QStringList>
#include <QWidget>

namespace keefi {
// Already interpreted observations only. This widget never reads a source,
// calculates a study, joins missing observations, or stores account data.
struct ChartLedgerRow {
    QString id, label, value, unit;
    int evidenceNumber = 0;
    QList<QPolygonF> segments;
};
class ChartMarketLedger final : public QWidget {
  public:
    explicit ChartMarketLedger(QWidget* parent = nullptr);
    void setRows(const QList<ChartLedgerRow>&);
  private:
    QStringList rowIds_;
};
}
