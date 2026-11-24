#pragma once
#include "keefi/MarketCompareSource.h"
#include <QHash>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QWidget>

class QButtonGroup;
class QComboBox;
class QLabel;
class QSettings;
class QPushButton;
class QTableWidget;

namespace keefi {
class CompareChartWidget;
class CompareCorrelationWidget;
class InfoButton;

// 시장 비교 탭: 금·원유·나스닥·비트코인을 같은 기준 시점의 % 변화로 겹치고,
// 기준 종목과 각 대상이 같이·따로·반대로 움직이는 정도를 보여 준다. 표시 전용.
class MarketCompareView final : public QWidget {
    Q_OBJECT
  public:
    explicit MarketCompareView(MarketCompareSource* source, QWidget* parent = nullptr);
    ~MarketCompareView() override;
    void refreshFromSource();
    void setSettings(QSettings*); // Borrowed public preferences; never opens a profile itself.
    QString legendText() const;  // Plain text for tests and accessibility.
    QString notesText() const;
    QList<qint64> relationChangeOpens() const; // Visible marker positions for inspection.
    qint64 baseOpenMs() const { return baseOpenMs_; }

  protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;

  private:
    void rebuild();
    void selectPeriod(const QString&);
    void clearPeriod();
    void setCorrelationExpanded(bool, bool persist);
    void saveHiddenSeries();
    QPointer<MarketCompareSource> source_;
    bool observingSource_ = false;
    QPointer<QSettings> settings_;
    QButtonGroup* timeframes_ = nullptr;
    QHash<QString,QPushButton*> periodButtons_;
    QPushButton* intervalMenu_ = nullptr;
    QString selectedPeriod_;
    bool automaticInterval_ = true;
    QComboBox* baseSelector_ = nullptr;
    QComboBox* targetSelector_ = nullptr;
    QComboBox* wonSelector_ = nullptr;
    InfoButton* information_ = nullptr;
    QLabel* summary_ = nullptr;
    QLabel* collectorStatus_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* correlationToggle_ = nullptr;
    QWidget* correlationControls_ = nullptr;
    QHash<QString,QPushButton*> chips_;
    QSet<QString> hiddenSeries_;
    QString legendText_, notesText_;
    CompareChartWidget* chart_ = nullptr;
    QTableWidget* periodTable_ = nullptr;
    CompareCorrelationWidget* correlation_ = nullptr;
    qint64 chosenBaseOpenMs_ = 0, baseOpenMs_ = 0;
    // Confirmed marks shown this session, per base|target|interval|their currencies.
    // Keep left-edge marks when refresh drops their warmup; never retain provisional FX.
    QHash<QString, QMap<qint64, RelationChange>> relationCache_;
    QString baseIdentity_ = QStringLiteral("coin:binance:BTCUSDT");
    QString targetIdentity_ = QStringLiteral("perp:binance-usdm:XAUUSDT");
};
} // namespace keefi
