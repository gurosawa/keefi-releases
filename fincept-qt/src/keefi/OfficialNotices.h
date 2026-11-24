#pragma once
#include "keefi/TradingTypes.h"
#include <QObject>
#include <functional>
#include <memory>

namespace keefi {
class TradingTransport;
enum class OfficialNoticeSource { Okx, Bithumb, Upbit };
struct OfficialNotice {
    QString title, url;
    QStringList categories;
    qint64 publishedAtMs = 0; // Only when the official source provides an absolute timestamp.
    QString publishedText, modifiedText; // Preserve timezone-free source times without guessing KST.
};
struct OfficialNoticeFeed {
    OfficialNoticeSource source = OfficialNoticeSource::Okx;
    bool available = false, loading = false;
    qint64 receivedAtMs = 0;
    QString reason;
    QList<OfficialNotice> notices;
};
// A realtime official source without a REST list (Upbit). Implemented outside this model.
class OfficialNoticeStream : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;
    virtual void setActive(bool) = 0;
    virtual OfficialNoticeFeed feed() const = 0;
  signals:
    void changed();
};
// Public official lists only. The APIs have no structured asset/chain identity,
// so title symbols never become a verified match to private holdings.
// Own a dedicated transport; cancellation must not interrupt other consumers.
class OfficialNotices final : public QObject {
    Q_OBJECT
  public:
    static constexpr qint64 FreshnessMs = 30 * 60000; // Local display policy, not a source latency guarantee.
    using Clock = std::function<qint64()>;
    explicit OfficialNotices(TradingTransport*, QObject* parent = nullptr, Clock clock = {});
    ~OfficialNotices() override;
    void setActive(bool);
    void refresh();
    // Upbit has no public notice list; its realtime socket joins only while attached.
    void setUpbitAnnouncements(OfficialNoticeStream*);
    OfficialNoticeFeed feed(OfficialNoticeSource, qint64 asOfMs = 0) const;
    static QString sourceName(OfficialNoticeSource);
    static QString matchingUnavailableReason(const QString& provider, const QString& market);
    static RequestSpec request(OfficialNoticeSource);
    static bool isRequest(const RequestSpec&);
    static bool allowedArticle(OfficialNoticeSource, const QUrl&);
    static bool parse(OfficialNoticeSource, const QByteArray&, qint64 receivedAtMs, OfficialNoticeFeed*);
  signals:
    void changed();
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
} // namespace keefi
