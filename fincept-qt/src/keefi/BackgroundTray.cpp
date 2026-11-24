#include "keefi/BackgroundTray.h"
#include <QIcon>
#include <QSystemTrayIcon>
namespace keefi {
namespace {
class QtBackgroundTray final:public BackgroundTray {
  public:
    QtBackgroundTray(const QIcon& icon,const QString& tooltip,QObject* parent)
        :BackgroundTray(parent),tray_(new QSystemTrayIcon(icon,this)){
        tray_->setToolTip(tooltip);
        connect(tray_,&QSystemTrayIcon::activated,this,[this](QSystemTrayIcon::ActivationReason reason){
            if(reason==QSystemTrayIcon::Trigger||reason==QSystemTrayIcon::DoubleClick)emit activated();
        });
    }
    bool available() const override {return QSystemTrayIcon::isSystemTrayAvailable();}
    bool visible() const override {return tray_->isVisible();}
    void setMenu(QMenu* menu) override {tray_->setContextMenu(menu);}
    void show() override {tray_->show();}
    void hide() override {tray_->hide();}
  private:
    QSystemTrayIcon* tray_;
};
}
BackgroundCloseDecision backgroundCloseDecision(bool quitting,bool preference,bool available,bool visible,bool hintSeen){
    if(!quitting&&preference&&available&&visible)return {BackgroundCloseAction::Hide,!hintSeen};
    return {};
}
BackgroundTray* makeBackgroundTray(const QIcon& icon,const QString& tooltip,QObject* parent){return new QtBackgroundTray(icon,tooltip,parent);}
} // namespace keefi
