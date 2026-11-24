#pragma once
#include "keefi/MarketCandles.h"
#include "keefi/ChartStudies.h"
#include "keefi/StockChartTypes.h"
#include "keefi/CoinFlowStudies.h"
#include "keefi/ChartDrawings.h"
#include "keefi/ChartShareCard.h"
#include "keefi/ChartSync.h"
#include "keefi/ExtendedChartStudies.h"
#include "keefi/MarketStudies.h"
#include "keefi/ChartPublicContextSource.h"
#include <QMap>
#include <QHash>
#include <QColor>
#include <QIcon>
#include <QPointer>
#include <QSet>
#include <QWidget>
#include <functional>
class QLabel;
class QGridLayout;
class QHBoxLayout;
class QVBoxLayout;
class QPushButton;
class QCheckBox;
class QComboBox;
class QFrame;
class QScrollArea;
class QToolButton;
class QKeyEvent;
class QResizeEvent;
class QDialog;
class QTimer;
namespace keefi {
class InfoButton;
class ChartStudyPreferences;
class StockChartSource;
class CoinFlowSource;
class CoinFlowStudyRunner;
class ChartDrawings;
class ChartInstrumentPresentation;
struct ChartInstrumentLabel;
struct CandleStudySet;
enum class ChartPriceKind { Average, Entry, Liquidation };
struct ChartPriceLine {
    QString provider,accountScope,asset,currency,price,sourceUrl;
    qint64 receivedAtMs=0;
    ChartPriceKind kind=ChartPriceKind::Average;
    int evidenceNumber=0;
};
class MarketCandlesView final:public QWidget {
    Q_OBJECT
  public:
    explicit MarketCandlesView(MarketCandles*,QWidget* parent=nullptr);
    ~MarketCandlesView() override;
    QString stockUnavailableReason() const;
    void setHeaderControls(QWidget*);
    // Standalone views retain their own header; workspace hosts reuse the same controls.
    void setTitleHost(QWidget*);
    void setToolbarHosts(QWidget* instrumentHost,QWidget* controlsHost,QWidget* menuAnchor=nullptr);
    void setRelatedMarketInformation(const QString&);
    void setAsset(const QString&);
    void setSelection(const QString& fullIdentity, qint64 intervalMs);
    // Presentation only: never changes selection, price evidence, calculations or requests.
    // A failure is shown only for the matching unavailable chart; empty input clears it.
    void setMetadataFailure(const QString& fullIdentity, const QString& reason);
    void setStockSource(StockChartSource*);
    void setCoinFlowSource(CoinFlowSource*);
    void setPublicContextSource(ChartPublicContextSource*);
    void setObservationStudies(const QSet<QString>&);
    void setScorecardStudy(const QString&);
    void setSupplementalStudies(const QList<ExtendedTimedDirection>&,const ExtendedLowVolumeZones&,const ExtendedRelativeStrength&);
    void setInstrumentPresentation(ChartInstrumentPresentation*);
    void setDrawingModel(ChartDrawings*);
    void setDrawingPublicContext(bool);
    void clearPrivateDrawingContext();
    void setDrawingAlertLists(const QMap<QString,QString>& names,const QSet<QString>& enabledLineLists={});
    CoinFlowStudySet coinFlowStudies() const;
    ExtendedStrongReversal strongReversalSnapshot() const;
    void setIncludeExtendedSessions(bool);
    void setIndicatorPreferences(ChartStudyPreferences*);
    void showIndicatorHelp(const QString& id,QWidget* returnFocus=nullptr);
    ChartStudySet chartStudies() const;
    ExtendedChartStudySet extendedStudies() const;
    QList<ChartIndicatorState> indicatorStates() const;
    QString indicatorContext() const;
    quint64 indicatorRevision() const;
    int pricePlotHeight() const;
    bool navigateToTime(qint64 originalUtcMs);
    void returnToLive();
    ChartShareViewport visibleViewport() const;
    ChartSyncViewport syncViewport() const;
    bool followsLive() const;
    int nominalZoomBars() const;
    void restoreLiveViewport(int zoomBars);
    void applySyncedViewport(const ChartSyncViewport&,bool matchRange);
    void setSyncedCrosshair(qint64 utcMs);
    ChartShareSnapshot shareSnapshot(const ChartShareOptions& options = {}, qint64 asOfMs = 0) const;
    void setPriceLines(const QList<ChartPriceLine>&);
    void setCandleEvidenceNumber(int);
    QList<ChartPriceLine> priceLines() const;
    void clearPrivate();
    // Chart drawing style ("candles", "bars", "line"); stored in the indicator preferences when set.
    QString chartStyle() const;
    void setChartStyle(const QString&);
    // 1 = automatic price range; changed by dragging the price axis.
    double priceScale() const;
  signals:
    void stockConnectionRequested(const QString& identity,qint64 intervalMs);
    void stockDailyRequested();
    void studiesChanged();
    void viewportChanged();
    void userViewportNavigated(keefi::ChartSyncViewport,bool zoom);
    void crosshairHovered(qint64 utcMs);
    void coinFlowStudiesChanged();
    void publicFlowObservationReady(const keefi::CoinFlowSnapshot&,const keefi::CoinFlowStudySet&,const keefi::ExtendedStrongReversal&);
    void drawingAlertSettingsRequested(const QString& ownerListId);
  protected:
    bool eventFilter(QObject*,QEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
  private:
    void refresh();
    void refreshLiveObservation();
    void refreshHistoryNavigation();
    ChartInstrumentLabel instrumentLabel() const;
    void refreshIndicatorUi();
    void refreshCoreStudySummary();
    void layoutCoreStudySummary();
    void layoutIndicatorChips();
    void showIndicatorPicker();
    void showIndicatorOverflow();
    void refreshLowerPane();
    void renderCoinFlow();
    void refreshPublicContext();
    void showLiquidationRecords();
    void maybeShowIndicatorIntro();
    void refreshInformation();
    void refreshStudies(const MarketCandleSeries&);
    void refreshDisplayStudies(const MarketCandleSeries&,const MarketCandleSeries&);
    void requestDisplayWarmup(const MarketCandleSeries&,const ExtendedChartOptions&);
    QByteArray calculationKey(const MarketCandleSeries&) const;
    ExtendedChartOptions extendedOptions() const;
    void invalidateCompletedStudies();
    void applyPendingCompletedStudies();
    void applyCompletedStudyDetails(const CandleCompression&,const CandleStudySet&);
    void refreshStock();
    void refreshCoinFlow();
    void syncDrawingContext(const MarketCandleSeries&);
    ChartDrawingFrame drawingFrame() const;
    void refreshDrawingUi();
    void layoutDrawingTools();
    void saveDrawingAlert();
    void runAutoChartGeometry();
    void applyAutoPreview();
    void clearAutoPreviewPanel();
    void setDrawingObjectsOpen(bool open);
    void layoutDrawingObjectsPanel();
    void syncDrawingObjectsPanel();
    bool drawingContextEditable() const;
    void showDrawingFeedback(const QString& text);
    void clearDrawingFeedback();
    QPointer<MarketCandles> model_;
    quint64 refreshRevision_ = 0;
    QPointer<ChartInstrumentPresentation> instrumentPresentation_;
    QPointer<StockChartSource> stockSource_;
    StockChartSnapshot stockSnapshot_;
    QPointer<CoinFlowSource> coinFlowSource_;
    CoinFlowStudySet coinFlowSnapshot_;
    CoinFlowStudyRunner* coinFlowRunner_=nullptr;
    QTimer* coinFlowRefresh_=nullptr;
    QPointer<ChartPublicContextSource> publicContextSource_;
    ChartPublicContext publicContextSnapshot_;
    ExtendedLevelSet levelsSnapshot_;
    QScrollArea* studyReadoutStrip_=nullptr;
    QMap<QString,QLabel*> studyReadouts_;
    QWidget* futuresLedger_=nullptr;
    QLabel* futuresLedgerText_=nullptr;
    QString coinFlowContext_,coinFlowStatus_;
    CoinFlowSnapshot coinFlowInput_;
    quint64 flowGeneration_=0;
    ExtendedAmountDelta deltaSnapshot_;
    ExtendedStrongReversal strongSnapshot_;
    ExtendedLowVolumeZones tradeZonesSnapshot_,minuteZonesSnapshot_;
    ExtendedRelativeStrength relativeSnapshot_;
    QList<ExtendedTimedDirection> higherTrends_;
    QByteArray supplementalKey_;
    QSet<QString> observationStudies_;
    QString scorecardStudy_;
    QPushButton* liquidationRecords_=nullptr;
    QByteArray coinFlowKey_;
    bool includeExtended_ = false;
    QPointer<ChartStudyPreferences> indicatorPreferences_;
    QString asset_, identity_, currency_;
    QString metadataFailureIdentity_, metadataFailureReason_;
    qint64 intervalMs_ = MarketCandles::IntervalMs;
    QList<ChartPriceLine> priceLines_;
    QPointer<QLabel> title_;
    QHBoxLayout* headingLayout_=nullptr;
    QPointer<InfoButton> information_;
    QString relatedMarketInformation_;
    QPointer<QWidget> toolbarMenuAnchor_;
    QPushButton *stockConnect_=nullptr,*stockDaily_=nullptr;
    QLabel *status_=nullptr,*study_=nullptr;
    QWidget* archiveNotice_=nullptr;
    QLabel* archiveMessage_=nullptr;
    QPushButton* archiveRetry_=nullptr;
    QWidget* chart_=nullptr;
    QLabel* historyStatus_=nullptr;
    QPushButton *historyRetry_=nullptr,*liveReturn_=nullptr;
    bool historyPrefetchQueued_=false;
    QWidget *indicatorLegend_=nullptr,*indicatorIntro_=nullptr;
    QGridLayout* indicatorLegendLayout_=nullptr;
    QWidget* coreStudySummary_=nullptr;
    QGridLayout* coreStudySummaryLayout_=nullptr;
    QMap<QString,QWidget*> coreStudyCards_;
    int coreStudyColumns_=0;
    QPushButton *indicatorPicker_=nullptr,*indicatorMore_=nullptr,*indicatorApplied_=nullptr;
    QWidget* lowerPane_=nullptr;
    QMap<QString,QWidget*> lowerStudies_;
    QMap<QString,QWidget*> indicatorChips_;
    QPointer<QDialog> indicatorHelpDialog_;
    QStringList legendIds_;
    QStringList laidOutLegendIds_;
    int legendColumns_=0;
    QColor indicatorIconInk_;
    QIcon indicatorEyeShownIcon_,indicatorEyeHiddenIcon_,indicatorOffIcon_;
    bool introDisplayed_=false,introDismissed_=false;
    bool studyDetailsExpanded_=false;
    ChartStudySet studySnapshot_;
    ExtendedChartStudySet extendedSnapshot_;
    // Drawing warm-up never becomes the observation input exposed to alerts/scorecards.
    ExtendedChartStudySet displayExtendedSnapshot_;
    QByteArray displayStudiesKey_;
    quint64 displayStudiesGeneration_=0;
    CandleCompression compressionSnapshot_;
    CandleStudySet explanationSnapshot_;
    QList<ChartCloudPoint> stockDisplayCloud_;
    CandleStudySet stockDisplayAdx_;
    QByteArray enabledStudyKey_;
    QByteArray studyKey_;
    quint64 studyGeneration_=0;
    quint64 canvasGeneration_=0,paintedCanvasGeneration_=0;
    std::function<void()> pendingStudyApply_;
    QByteArray detailStudyKey_;
    QString detailStudyCurrency_;
    QLabel* similarityEvidence_=nullptr;
    QLabel* pullbackEvidence_=nullptr;
    QLabel *stockContext_=nullptr,*stockObservations_=nullptr;
    QLabel *coinFlowDetails_=nullptr,*coinFlowScorecard_=nullptr;
    QWidget* coinFlowDetailsPane_=nullptr;
    QPointer<ChartDrawings> drawingModel_;
    MarketCandleSeries drawingSeries_;
    bool drawingPublicContext_=false,drawingPrivateCleared_=false;
    QWidget *drawingToolbar_=nullptr,*drawingLinePanel_=nullptr;
    QGridLayout* drawingToolbarLayout_=nullptr;
    QMap<ChartDrawingTool,QToolButton*> drawingTools_;
    QToolButton* drawingMore_=nullptr;
    QToolButton *drawingAuto_=nullptr,*drawingObjects_=nullptr;
    QToolButton *drawingMagnet_=nullptr,*drawingVisible_=nullptr,*drawingLocked_=nullptr,*drawingClear_=nullptr;
    QToolButton *drawingUndo_=nullptr,*drawingRedo_=nullptr,*chartStyleButton_=nullptr;
    QString chartStyleIconKey_;
    QFrame* drawingObjectsPanel_=nullptr;
    QScrollArea* drawingObjectsScroll_=nullptr;
    QWidget *drawingPreviewGroup_=nullptr,*drawingSavedGroup_=nullptr;
    QVBoxLayout *drawingPreviewLayout_=nullptr,*drawingSavedLayout_=nullptr;
    QPushButton* drawingPreviewApply_=nullptr;
    QLabel *drawingObjectsTitle_=nullptr,*drawingObjectsEmpty_=nullptr;
    struct PreviewRow {
        QWidget* row=nullptr;
        QCheckBox* check=nullptr;
        QString key;
        ChartDrawingKind kind=ChartDrawingKind::Horizontal;
        QList<ChartDrawingPoint> anchors;
    };
    QList<PreviewRow> drawingPreviewRows_;
    struct SavedDrawingRow {
        QWidget* row=nullptr;
        QLabel* label=nullptr;
        QToolButton *eye=nullptr,*lock=nullptr,*remove=nullptr;
        QString id;
    };
    QHash<QString,SavedDrawingRow> drawingObjectRows_;
    QColor drawingObjectIconInk_,drawingObjectAccentInk_;
    QIcon drawingEyeShownIcon_,drawingEyeHiddenIcon_,drawingLockedIcon_,drawingUnlockedIcon_,drawingTrashIcon_;
    void undoDrawing(bool redo);
    void refreshChartStyleButton();
    QLabel *drawingStatus_=nullptr,*drawingLineStatus_=nullptr;
    QString drawingFeedbackText_;
    quint64 drawingFeedbackGen_=0;
    QTimer* drawingFeedbackTimer_=nullptr;
    QCheckBox* drawingLineEnabled_=nullptr;
    QComboBox *drawingLineOwner_=nullptr,*drawingLineInterval_=nullptr;
    QPushButton *drawingLineSettings_=nullptr,*drawingLineDelete_=nullptr;
    QMap<QString,QString> drawingAlertListNames_;
    QSet<QString> drawingEnabledLineLists_;
    QString drawingUiSelection_;
    int drawingToolbarColumns_=0;
};
}
