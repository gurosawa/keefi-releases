#pragma once
#include <QMap>
#include <QWidget>
#include <memory>
namespace keefi {
class ChartAlertEngine;
class ChartAlertFeed;
class SystemNotificationSink;
class ChartPhrasePack;
// Presentation and public UI preferences only. Delivery, source collection and
// private epochs stay in their injected modules; no constructor profile reads.
class ChartAlertsPanel final:public QWidget {
    Q_OBJECT
  public:
    ChartAlertsPanel(ChartAlertEngine*,ChartAlertFeed*,SystemNotificationSink*,ChartPhrasePack*,QWidget* parent=nullptr);
    ~ChartAlertsPanel() override;
    void setListLabels(const QMap<QString,QString>&);
    // This object belongs to the app window. Option pages are disposable views;
    // closing one must never discard accepted history, preferences or engine state.
    QWidget* createSettingsPage(QWidget* parent,const QString& listId={});
    void selectSettingsList(const QString& listId);
    void openSettings(const QString& listId={});
    void reviewPhrasePack(const QByteArray&);
    void clearPrivate();
    void setBackgroundEnabled(bool);
    void setBackgroundAvailable(bool); // Runtime capability; never persisted.
    bool backgroundEnabled() const;
    bool closeHintSeen() const;
    void markCloseHintSeen();
    QByteArray publicState() const;
    bool restorePublicState(const QByteArray&,QString* error=nullptr);
  signals:
    void publicStateChanged();
    void settingsRequested(const QString& listId);
    void generalSettingsRequested();
    void backgroundEnabledChanged(bool);
  protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
  private:
    struct Private;std::unique_ptr<Private> d_;
};
}
