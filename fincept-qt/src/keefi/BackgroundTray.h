#pragma once
#include <QObject>
#include <QString>
class QIcon;
class QMenu;

namespace keefi {
class BackgroundTray:public QObject {
    Q_OBJECT
  public:
    explicit BackgroundTray(QObject* parent=nullptr):QObject(parent){}
    ~BackgroundTray() override=default;
    virtual bool available() const=0;
    virtual bool visible() const=0;
    virtual void setMenu(QMenu*)=0; // Borrowed menu; the caller owns its lifetime.
    virtual void show()=0;
    virtual void hide()=0;
  signals:
    void activated();
};
enum class BackgroundCloseAction {Hide,Quit};
struct BackgroundCloseDecision {
    BackgroundCloseAction action=BackgroundCloseAction::Quit;
    bool showExplanation=false;
};
BackgroundCloseDecision backgroundCloseDecision(bool quitting,bool preference,bool available,bool visible,bool hintSeen);
// Only app construction uses the native factory. Tests inject a fake facade.
BackgroundTray* makeBackgroundTray(const QIcon&,const QString& tooltip,QObject* parent=nullptr);
} // namespace keefi
