#pragma once
#include "keefi/ChartStudyFeed.h"
#include "keefi/ChartSync.h"
#include <QPointer>
#include <QWidget>
class QGridLayout;
class QPushButton;
class QComboBox;
class QLabel;
class QTimer;
namespace keefi {
// Two public spot panels. The existing full chart is placed over the active
// panel without reparenting; only passive panels ask the shared REST feed.
class ChartGridView final : public QWidget {
    Q_OBJECT
  public:
    explicit ChartGridView(QWidget* parent=nullptr);
    ~ChartGridView() override;
    void setSource(ChartStudyFeed*);
    void setPanelCount(int);
    void setActiveView(QWidget*);
    void seed(const QStringList& publicIdentities);
    void setSelection(const QString&,qint64);
    void rollbackActivation(const QString& previousIdentity,qint64 previousInterval);
    ChartSyncSettings syncSettings() const { return sync_; }
    void setSyncOption(ChartSyncOption,bool);
    // A passive programmatic observation never becomes a user navigation event.
    void observeActiveViewport(const ChartSyncViewport&,bool followsLive=false,int zoomBars=100);
    void activeViewportNavigated(const ChartSyncViewport&,bool zoom);
    void activeCrosshairHovered(qint64 utcMs);
    QString panelIdentity(int) const;
    qint64 panelInterval(int) const;
    ChartSyncViewport panelViewport(int) const;
  signals:
    void activated(QString identity,qint64 intervalMs);
    void activeViewportSyncRequested(keefi::ChartSyncViewport,bool matchRange);
    void activeViewportFollowLiveRequested(int zoomBars);
    void activeCrosshairSyncRequested(qint64 utcMs);
    void syncSettingsChanged();
  protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void resizeEvent(QResizeEvent*) override;
  private:
    struct Panel {
        QWidget* box=nullptr;
        QPushButton* select=nullptr;
        QComboBox* period=nullptr;
        QWidget* canvas=nullptr;
        QLabel* status=nullptr;
        QString identity;
        qint64 interval=900000;
        ChartSyncViewport viewport;
        bool followsLive=true;
        int zoomBars=100;
    };
    void activate(int);
    void syncSelection(int source,bool symbolChanged,bool intervalChanged);
    void syncViewport(int source,const ChartSyncViewport&,bool zoom);
    void syncCrosshair(int source,qint64 utcMs);
    void clearSyncCrosshairs();
    void syncDemand();
    void refresh();
    void syncActiveGeometry();
    QList<Panel> panels_;
    QGridLayout* layout_=nullptr;
    QPointer<ChartStudyFeed> source_;
    QPointer<QWidget> activeView_;
    QTimer* freshness_=nullptr;
    int count_=0,active_=0,previousActive_=0;
    ChartSyncSettings sync_;
    quint64 viewportApplyCount_=0,crosshairEventCount_=0;
};
}
