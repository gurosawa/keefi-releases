#pragma once
#include "keefi/CoinFlowStudies.h"
#include "keefi/ExtendedChartStudies.h"
#include <QObject>
#include <functional>
#include <memory>

namespace keefi {
// One in-flight immutable public calculation and one newest pending snapshot.
// No worker captures a QObject, transport, widget, profile, or account value.
class CoinFlowStudyRunner final : public QObject {
    Q_OBJECT
  public:
    using Calculator=std::function<CoinFlowStudySet(const CoinFlowSnapshot&,qint64)>;
    explicit CoinFlowStudyRunner(QObject* parent=nullptr,Calculator={});
    ~CoinFlowStudyRunner() override;
    void request(const CoinFlowSnapshot&,qint64 asOfMs,const QString& contextKey,const QSet<QString>& enabledStudies={});
    void invalidate();
    void setEnabled(bool);
    bool pending() const;
  signals:
    void resultReady(const QString& contextKey,const keefi::CoinFlowStudySet&,qint64 calculationNs);
    void calculatedResultReady(const keefi::CoinFlowSnapshot&,const QString& contextKey,const keefi::CoinFlowStudySet&);
    void observationReady(const keefi::CoinFlowSnapshot&,const QString& contextKey,const keefi::CoinFlowStudySet&,const keefi::ExtendedStrongReversal&);
    void extendedResultReady(const QString& contextKey,const keefi::ExtendedAmountDelta&,
        const keefi::ExtendedLowVolumeZones&,const keefi::ExtendedStrongReversal&);
  private:
    struct Private;std::unique_ptr<Private> d_;
};
}
