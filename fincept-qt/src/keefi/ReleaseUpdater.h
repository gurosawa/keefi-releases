#pragma once

#include "ReleaseUpdateManifest.h"
#include <QCryptographicHash>
#include <QFile>
#include <QObject>
#include <QPointer>
#include <QTemporaryDir>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
namespace keefi {
class ReleaseUpdater final : public QObject {
    Q_OBJECT
public:
    ReleaseUpdater(QString installedVersion, int installedBuild, QString platform,
                   QNetworkAccessManager* network, QByteArray publicKey, QObject* parent = nullptr);
    enum class State { Idle, Checking, Available, Downloading, Ready, Current, Failed };
    State state() const { return state_; }
    QString status() const { return status_; }
    ReleaseUpdateAsset asset() const { return asset_; }
    QString archivePath() const { return archive_.fileName(); }
    void check();
    void download();
    void refuse(const QString& error);
    void handoffComplete();
signals:
    void changed();
    void archiveReady();
private:
    enum class Part { Releases, Manifest, Signature, Archive };
    void fetch(Part part, const QUrl& url, int redirects = 0);
    void consume(QNetworkReply* reply, Part part);
    void received(QNetworkReply* reply, Part part, int redirects);
    void change(State state, const QString& text);
    QUrl assetUrl(const QString& name) const;
    QString installedVersion_, platform_, selectedTag_, status_;
    int installedBuild_;
    QNetworkAccessManager* network_;
    QByteArray publicKey_, body_, manifest_;
    QPointer<QNetworkReply> active_;
    ReleaseUpdateAsset asset_;
    std::unique_ptr<QTemporaryDir> staging_;
    QFile archive_;
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    qint64 received_ = 0;
    State state_ = State::Idle;
};
}
