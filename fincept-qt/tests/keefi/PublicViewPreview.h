#pragma once
#include "keefi/AccountModel.h"
#include "keefi/BackgroundTray.h"
#include "keefi/LocalWindow.h"
#include "keefi/Vault.h"
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QPixmap>
#include <QTemporaryDir>
#include <QWidget>
namespace keefi::test {
// Opt-in synthetic public fixtures only; the donor uses an empty temporary
// profile and an unavailable tray. Never capture the running user's window.
inline bool savePublicViewPreview(QWidget& view,const QString& stem){
    const auto destination=qEnvironmentVariable("KEEFI_G8_CHART_PREVIEW");if(destination.isEmpty())return true;
    QTemporaryDir profile;if(!profile.isValid()||!QDir().mkpath(destination))return false;
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,profile.filePath("settings"));
    class UnavailableTray final:public BackgroundTray{public:bool available()const override{return false;}bool visible()const override{return false;}void setMenu(QMenu*)override{}void show()override{}void hide()override{}} tray;
    Vault vault(profile.filePath("synthetic-c9-theme-vault"));AccountModel accounts(&vault);LocalWindow donor(vault,accounts,nullptr,&tray);view.setStyleSheet(donor.styleSheet());
    view.resize(1280,800);QCoreApplication::processEvents();const bool wide=view.grab().save(QDir(destination).filePath(stem+"-wide.png"));
    view.resize(820,1000);QCoreApplication::processEvents();return view.grab().save(QDir(destination).filePath(stem+"-narrow.png"))&&wide;
}
}
