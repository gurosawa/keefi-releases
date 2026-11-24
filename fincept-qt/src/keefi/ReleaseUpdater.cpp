#include "ReleaseUpdater.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

namespace keefi {
namespace {
const QString ReleaseList = QStringLiteral("https://api.github.com/repos/gurosawa/keefi-releases/releases?per_page=30");
const QString Downloads = QStringLiteral("https://github.com/gurosawa/keefi-releases/releases/download/");
const QString Manifest = QStringLiteral("keefi-update.json");
const QString Signature = QStringLiteral("keefi-update.json.sig");

bool githubAssetRedirect(const QUrl& url) {
    return url.isValid() && url.scheme() == "https" && url.userInfo().isEmpty() && url.port(443) == 443
        && (url.host() == "release-assets.githubusercontent.com" || url.host() == "objects.githubusercontent.com");
}
}

ReleaseUpdater::ReleaseUpdater(QString installedVersion, int installedBuild, QString platform,
                               QNetworkAccessManager* network, QByteArray publicKey, QObject* parent)
    : QObject(parent), installedVersion_(std::move(installedVersion)), platform_(std::move(platform)),
      installedBuild_(installedBuild), network_(network), publicKey_(std::move(publicKey)) {
    status_ = tr("현재 버전 %1 (%2)").arg(installedVersion_).arg(installedBuild_);
}

void ReleaseUpdater::change(State state, const QString& text) {
    state_ = state;
    status_ = text;
    emit changed();
}

void ReleaseUpdater::refuse(const QString& error) {
    if (active_) {
        auto* reply = active_.data();
        active_.clear();
        reply->abort();
    }
    archive_.close();
    staging_.reset();
    change(State::Failed, error);
}

void ReleaseUpdater::check() {
    if (state_ == State::Checking || state_ == State::Downloading || state_ == State::Ready) return;
    asset_ = {};
    manifest_.clear();
    selectedTag_.clear();
    change(State::Checking, tr("GitHub에서 업데이트를 확인하고 있습니다."));
    fetch(Part::Releases, QUrl(ReleaseList));
}

QUrl ReleaseUpdater::assetUrl(const QString& name) const {
    return QUrl(Downloads + selectedTag_ + '/' + name);
}

void ReleaseUpdater::download() {
    if (state_ != State::Available) return;
    staging_ = std::make_unique<QTemporaryDir>(QDir::temp().filePath(QStringLiteral("KEEFI-update-XXXXXX")));
    if (!staging_->isValid()) return refuse(tr("업데이트를 준비할 임시 폴더를 만들 수 없습니다."));
    archive_.setFileName(staging_->filePath(asset_.name));
    if (!archive_.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return refuse(tr("업데이트 파일을 저장할 수 없습니다."));
    received_ = 0;
    hash_.reset();
    change(State::Downloading, tr("%1 다운로드 중…").arg(asset_.version));
    fetch(Part::Archive, assetUrl(asset_.name));
}

void ReleaseUpdater::fetch(Part part, const QUrl& url, int redirects) {
    body_.clear();
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "KEEFI-Desktop-Updater/1");
    request.setRawHeader("Accept", part == Part::Releases ? "application/vnd.github+json" : "application/octet-stream");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setTransferTimeout(30000);
    auto* reply = network_->get(request);
    active_ = reply;
    reply->setReadBufferSize(128 * 1024);
    QTimer::singleShot(part == Part::Archive ? 300000 : 45000, reply, [reply] { if (reply->isRunning()) reply->abort(); });
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, part] { consume(reply, part); });
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    connect(reply, &QNetworkReply::finished, this, [this, reply, part, redirects] { received(reply, part, redirects); });
}

void ReleaseUpdater::consume(QNetworkReply* reply, Part part) {
    if (active_ != reply) return;
    // Redirect and error responses can contain HTML. Never append it to the signed asset.
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (!status) return;
    while (reply->bytesAvailable() > 0) {
        const auto chunk = reply->read(64 * 1024);
        if (chunk.isEmpty()) break;
        if (status != 200) continue;
        if (part == Part::Archive) {
            if (received_ + chunk.size() > asset_.size || archive_.write(chunk) != chunk.size())
                return refuse(tr("업데이트 다운로드 크기 또는 저장 상태를 확인할 수 없습니다."));
            hash_.addData(chunk);
            received_ += chunk.size();
        } else {
            const qsizetype limit = part == Part::Releases ? 2 * 1024 * 1024 : part == Part::Manifest ? 32768 : 256;
            if (body_.size() + chunk.size() > limit) return refuse(tr("업데이트 응답이 허용 크기를 초과했습니다."));
            body_ += chunk;
        }
    }
}

void ReleaseUpdater::received(QNetworkReply* reply, Part part, int redirects) {
    if (active_ != reply) return;
    consume(reply, part);
    if (active_ != reply) return;
    active_.clear();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 300 && status < 400) {
        const auto location = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        const auto next = reply->url().resolved(location);
        if (redirects >= 3 || part == Part::Releases || location.isEmpty() || !githubAssetRedirect(next))
            return refuse(tr("배포 파일의 다운로드 주소를 확인할 수 없습니다."));
        return fetch(part, next, redirects + 1);
    }
    if (status != 200 || reply->error() != QNetworkReply::NoError)
        return refuse(tr("GitHub에 연결하지 못했습니다. 잠시 후 다시 확인해 주세요."));

    if (part == Part::Releases) {
        const auto list = QJsonDocument::fromJson(body_);
        if (!list.isArray()) return refuse(tr("GitHub 릴리스 목록을 읽을 수 없습니다."));
        static const QRegularExpression validTag(QStringLiteral(R"(^v[0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6}(?:-rc\.[0-9]{1,9})?$)"));
        for (const auto& item : list.array()) {
            const auto release = item.toObject();
            const auto tag = release.value("tag_name").toString();
            if (release.value("draft").toBool(true) || !validTag.match(tag).hasMatch()) continue;
            bool hasManifest = false, hasSignature = false;
            for (const auto& asset : release.value("assets").toArray()) {
                const auto name = asset.toObject().value("name").toString();
                hasManifest |= name == Manifest;
                hasSignature |= name == Signature;
            }
            if (!hasManifest && !hasSignature) continue; // Releases made before the updater.
            if (!hasManifest || !hasSignature) return refuse(tr("릴리스의 서명 파일이 완전하지 않습니다."));
            selectedTag_ = tag;
            return fetch(Part::Manifest, assetUrl(Manifest));
        }
        return change(State::Current, tr("업데이트할 서명된 릴리스가 없습니다."));
    }
    if (part == Part::Manifest) {
        manifest_ = body_;
        return fetch(Part::Signature, assetUrl(Signature));
    }
    if (part == Part::Signature) {
        QString error;
        const auto eligibility = verifyReleaseManifest(manifest_, body_, publicKey_, selectedTag_, platform_,
                                                      installedVersion_, installedBuild_, &asset_, &error);
        if (eligibility == UpdateEligibility::Invalid) return refuse(error);
        if (eligibility == UpdateEligibility::AlreadyCurrent)
            return change(State::Current, tr("현재 설치된 버전이 최신입니다."));
        return change(State::Available, tr("%1 (%2) 업데이트를 사용할 수 있습니다.").arg(asset_.version).arg(asset_.build));
    }
    if (received_ != asset_.size || hash_.result().toHex() != asset_.sha256Hex || !archive_.flush())
        return refuse(tr("다운로드한 파일이 서명된 배포본과 일치하지 않습니다."));
    archive_.close();
    change(State::Ready, tr("검증이 끝났습니다. 작업을 저장하고 다시 시작합니다."));
    emit archiveReady();
}

void ReleaseUpdater::handoffComplete() {
    if (staging_) staging_->setAutoRemove(false); // Installer owns the verified ZIP after the parent exits.
}
}
