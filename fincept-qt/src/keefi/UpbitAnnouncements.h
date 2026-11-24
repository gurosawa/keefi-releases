#pragma once
#include "keefi/OfficialNotices.h"
#include <QNetworkRequest>
#include <QObject>
#include <functional>
#include <memory>

namespace keefi {
class Vault;
// Private Upbit socket used only for the public announcement type.
class UpbitAnnouncementSocket : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;
    virtual void open(const QNetworkRequest&) = 0;
    virtual void send(const QByteArray&) = 0;
    virtual void abort() = 0;
  signals:
    void connected();
    void received(const QByteArray&);
    void failed();
};
// Official Upbit announcement WebSocket. It delivers only notices created or
// updated after connecting, so the list starts empty each session. The vault's
// read-only Upbit key signs one JWT per connection; nothing else is requested.
class UpbitAnnouncements final : public OfficialNoticeStream {
    Q_OBJECT
  public:
    using Clock = std::function<qint64()>;
    static constexpr int MaximumNotices = 20;
    explicit UpbitAnnouncements(Vault&, QObject* parent = nullptr);
    UpbitAnnouncements(Vault&, std::unique_ptr<UpbitAnnouncementSocket>, Clock = {}, QObject* parent = nullptr);
    ~UpbitAnnouncements() override;
    void setActive(bool) override;
    OfficialNoticeFeed feed() const override;
    static QUrl endpoint();
    static QByteArray subscription(const QString& ticket);
    // One DEFAULT-format announcement message; uuid is the replace key for UPDATED.
    static bool parse(const QByteArray&, OfficialNotice*, QString* uuid);
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
} // namespace keefi
