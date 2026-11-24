#pragma once
#include <QColor>
#include <QMap>
#include <QObject>
#include "keefi/ExtendedChartStudies.h"
namespace keefi {
struct ChartStudySettings {
    int rsiPeriod = 14;
    int macdFast = 12, macdSlow = 26, macdSignal = 9;
    int displayBandPeriod = ChartDisplayBandPeriod;
    double displayBandDeviations = ChartDisplayBandDeviations;
    QMap<QString, QColor> colors; // Omitted keys follow the theme.
    bool operator==(const ChartStudySettings&) const = default;
};
struct ChartStudyPreference {
    bool enabled=true,visible=true;
    bool operator==(const ChartStudyPreference&) const = default;
};
// Public display choices only: no identity, prices, account, or alert state.
class ChartStudyPreferences final:public QObject {
    Q_OBJECT
  public:
    explicit ChartStudyPreferences(QObject* parent=nullptr);
    QStringList supportedIds(const QString& market) const;
    bool enabled(const QString& id) const;
    bool visible(const QString& id) const;
    bool favorite(const QString& id) const;
    bool setFavorite(const QString& id,bool);
    bool setEnabled(const QString& id,bool);
    bool setVisible(const QString& id,bool);
    QStringList enabledOrder(const QString& market) const;
    QStringList visiblePanes(const QString& market) const;
    QList<ExtendedMaSpec> movingAverages() const;
    const ChartStudySettings& studySettings() const { return settings_; }
    QColor lineColor(const QString& key, const QColor& fallback) const { return settings_.colors.value(key, fallback); }
    static QStringList colorKeys(const QString& study);
    bool setStudySettings(const ChartStudySettings&, const QList<ExtendedMaSpec>&);
    bool setMovingAverages(const QList<ExtendedMaSpec>&);
    static bool isPane(const QString& id);
    bool introSeen() const;
    void markIntroSeen();
    // Price drawing style of the main chart: "candles" (default), "bars" (OHLC) or "line" (closes).
    QString chartStyle() const;
    bool setChartStyle(const QString&);
    static bool isChartStyle(const QString&);
    QByteArray publicState() const;
    bool restorePublicState(const QByteArray&,QString* reason=nullptr);
  signals:
    void changed();
  private:
    QMap<QString,ChartStudyPreference> values_;
    QStringList order_, favorites_;
    ChartStudySettings settings_;
    QList<ExtendedMaSpec> averages_=defaultChartMovingAverages();
    bool introSeen_=false;
    QString chartStyle_=QStringLiteral("candles");
};
}
