#include "keefi/UpbitAnnouncements.h"
#include "keefi/AccountModel.h"
#include "keefi/Vault.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
using namespace keefi;
namespace {
const QString Password = "Fake Upbit Notice Password! 2026";
bool prepare(Vault& vault, QString provider = "upbit") {
    AccountRecord a; a.provider = provider; a.market = "spot"; a.environment = "live"; a.alias = "fake notices"; a.currency = "KRW";
    return vault.create(Password) && vault.addAccount(a, {{"access_key", "FakeUpbitAccess123"}, {"secret", "FakeUpbitSecret123456"}}, StorageMode::EncryptedVault);
}
class FakeSocket final : public UpbitAnnouncementSocket {
  public:
    QList<QNetworkRequest> opened; QList<QByteArray> sent; int aborts = 0;
    void open(const QNetworkRequest& request) override { opened.append(request); }
    void send(const QByteArray& text) override { sent.append(text); }
    void abort() override { ++aborts; }
    void connect() { emit connected(); }
    void message(const QJsonObject& object) { emit received(QJsonDocument(object).toJson(QJsonDocument::Compact)); }
    void drop() { emit failed(); }
};
QJsonObject announcement(const QString& uuid, const QString& title, const QString& listed, const QString& event = "CREATED",
                         const QString& url = "https://upbit.com/service_center/notice?id=5001", const QString& category = "trade") {
    return {{"type", "announcement"}, {"event_type", event}, {"uuid", uuid}, {"title", title}, {"category", category}, {"url", url},
            {"first_listed_at", listed}, {"listed_at", listed}, {"timestamp", 1793145600000.0}, {"stream_type", "REALTIME"}};
}
}
class UpbitAnnouncementsTest final : public QObject {
    Q_OBJECT
  private slots:
    void maximumSizedStatusAndNoticeKeepNormalBehavior() {
        constexpr qsizetype Limit = 64 * 1024;
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(prepare(vault));
        qint64 now = 1793145600000;
        auto socket = std::make_unique<FakeSocket>(); auto* fake = socket.get();
        UpbitAnnouncements stream(vault, std::move(socket), [&] { return now; });
        stream.setActive(true); fake->connect();
        fake->message(announcement("kept", "kept", "2026-10-28T09:00:00"));
        const auto before = stream.feed(); const auto aborts = fake->aborts;
        QSignalSpy changed(&stream, &OfficialNoticeStream::changed);
        const QByteArray prefix = "{\"status\":\"UP\",\"padding\":\"";
        const QByteArray suffix = "\"}";
        const auto status = prefix + QByteArray(Limit - prefix.size() - suffix.size(), 'x') + suffix;
        QCOMPARE(status.size(), Limit);
        now += 1000; emit fake->received(status);
        auto after = stream.feed();
        QVERIFY(after.available); QVERIFY(!after.loading); QVERIFY(after.reason.isEmpty());
        QCOMPARE(after.receivedAtMs, now); QCOMPARE(after.notices.size(), before.notices.size());
        QCOMPARE(after.notices[0].title, before.notices[0].title);
        QCOMPARE(fake->aborts, aborts); QCOMPARE(changed.count(), 0); // Status UP emits no changed signal.
        auto object = announcement("boundary", "boundary notice", "2026-10-28T10:00:00");
        object.insert("padding", QString{});
        const auto baseSize = QJsonDocument(object).toJson(QJsonDocument::Compact).size();
        QVERIFY(baseSize < Limit);
        object.insert("padding", QString(Limit - baseSize, QChar('x')));
        const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        QCOMPARE(bytes.size(), Limit);
        now += 1000; emit fake->received(bytes);
        after = stream.feed();
        QVERIFY(after.available); QVERIFY(after.reason.isEmpty()); QCOMPARE(after.receivedAtMs, now);
        QCOMPARE(after.notices.size(), 2); QCOMPARE(after.notices[0].title, QString("boundary notice"));
        QCOMPARE(after.notices[1].title, QString("kept")); QCOMPARE(changed.count(), 1);
        QCOMPARE(fake->aborts, aborts); QCOMPARE(fake->opened.size(), 1); QCOMPARE(fake->sent.size(), 1);
        stream.setActive(false);
    }

    void announcementParseSizeBoundaryPreservesOutputs() {
        constexpr qsizetype Limit = 64 * 1024;
        auto object = announcement("boundary", "boundary notice", "2026-10-28T10:00:00");
        object.insert("padding", QString{});
        const auto baseSize = QJsonDocument(object).toJson(QJsonDocument::Compact).size();
        QVERIFY(baseSize < Limit);
        object.insert("padding", QString(Limit - baseSize, QChar('x')));
        auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        QCOMPARE(bytes.size(), Limit);
        OfficialNotice notice; QString uuid;
        QVERIFY(UpbitAnnouncements::parse(bytes, &notice, &uuid));
        QCOMPARE(uuid, QString("boundary")); QCOMPARE(notice.title, QString("boundary notice"));
        const auto before = notice; const auto beforeUuid = uuid;
        object.insert("padding", object.value("padding").toString() + QChar('x'));
        bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        QCOMPARE(bytes.size(), Limit + 1);
        QVERIFY(!UpbitAnnouncements::parse(bytes, &notice, &uuid));
        QCOMPARE(uuid, beforeUuid); QCOMPARE(notice.title, before.title); QCOMPARE(notice.url, before.url);
        QCOMPARE(notice.categories, before.categories); QCOMPARE(notice.publishedAtMs, before.publishedAtMs);
        QCOMPARE(notice.publishedText, before.publishedText); QCOMPARE(notice.modifiedText, before.modifiedText);
    }

    void oversizedStatusKeepsThePriorFeedAndRetries() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(prepare(vault));
        qint64 now = 1793145600000;
        auto socket = std::make_unique<FakeSocket>(); auto* fake = socket.get();
        UpbitAnnouncements stream(vault, std::move(socket), [&] { return now; });
        stream.setActive(true); fake->connect();
        fake->message(announcement("kept", "kept", "2026-10-28T09:00:00"));
        const auto before = stream.feed();
        QVERIFY(before.available); QCOMPARE(before.notices.size(), 1);
        const auto aborts = fake->aborts;
        QSignalSpy changed(&stream, &OfficialNoticeStream::changed);
        now += 1000;
        const QByteArray prefix = "{\"status\":\"UP\",\"padding\":\"";
        const QByteArray suffix = "\"}";
        const auto bytes = prefix + QByteArray(64 * 1024 + 1 - prefix.size() - suffix.size(), 'x') + suffix;
        QCOMPARE(bytes.size(), qsizetype(64 * 1024 + 1));
        emit fake->received(bytes);
        const auto after = stream.feed();
        // The unbounded status shortcut previously refreshed this timestamp.
        QCOMPARE(after.receivedAtMs, before.receivedAtMs);
        QVERIFY(!after.available); QVERIFY(!after.loading);
        QCOMPARE(after.reason, QStringLiteral("업비트 공지 연결 끊김 · 다시 연결 중"));
        QCOMPARE(fake->aborts, aborts + 1);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(after.notices.size(), before.notices.size());
        QCOMPARE(after.notices[0].title, before.notices[0].title);
        QCOMPARE(after.notices[0].url, before.notices[0].url);
        QCOMPARE(after.notices[0].categories, before.notices[0].categories);
        QCOMPARE(after.notices[0].publishedAtMs, before.notices[0].publishedAtMs);
        QCOMPARE(after.notices[0].publishedText, before.notices[0].publishedText);
        QCOMPARE(after.notices[0].modifiedText, before.notices[0].modifiedText);
        fake->drop(); // A later socket error must not count the same failure twice.
        QCOMPARE(fake->aborts, aborts + 1); QCOMPARE(changed.count(), 1);
        now += 1000;
        emit fake->received("{\"status\":\"UP\"}");
        QCOMPARE(stream.feed().receivedAtMs, before.receivedAtMs);
        QTRY_COMPARE_WITH_TIMEOUT(fake->opened.size(), 2, 7000);
        QVERIFY(stream.feed().loading); QVERIFY(!stream.feed().available);
        QCOMPARE(stream.feed().receivedAtMs, before.receivedAtMs);
        now += 1000; fake->connect();
        QVERIFY(stream.feed().available); QCOMPARE(stream.feed().receivedAtMs, now);
        QCOMPARE(stream.feed().notices.size(), 1); QCOMPARE(fake->sent.size(), 2);
        stream.setActive(false);
    }

    void signsOneJwtWithTheVaultKeyAndSubscribesToTradeAndWalletOnly() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(prepare(vault));
        auto socket = std::make_unique<FakeSocket>(); auto* fake = socket.get();
        UpbitAnnouncements stream(vault, std::move(socket));
        QVERIFY(fake->opened.isEmpty()); QVERIFY(!stream.feed().available);
        stream.setActive(true); QCOMPARE(fake->opened.size(), 1);
        const auto request = fake->opened.first(); QCOMPARE(request.url(), QUrl("wss://api.upbit.com/websocket/v1/private"));
        const auto header = request.rawHeader("Authorization"); QVERIFY(header.startsWith("Bearer "));
        const auto parts = header.mid(7).split('.'); QCOMPARE(parts.size(), 3);
        const auto payload = QJsonDocument::fromJson(QByteArray::fromBase64(parts[1], QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)).object();
        QCOMPARE(payload.value("access_key").toString(), QString("FakeUpbitAccess123")); QVERIFY(!payload.contains("query_hash"));
        QVERIFY(!header.contains("FakeUpbitSecret")); QVERIFY(fake->sent.isEmpty());
        fake->connect(); QVERIFY(stream.feed().available); QVERIFY(stream.feed().notices.isEmpty());
        QCOMPARE(fake->sent.size(), 1);
        const auto message = QJsonDocument::fromJson(fake->sent.first()).array(); QCOMPARE(message.size(), 3);
        QVERIFY(message[0].toObject().value("ticket").toString().startsWith("keefi-notice-"));
        QCOMPARE(message[1].toObject().value("type").toString(), QString("announcement"));
        QCOMPARE(message[1].toObject().value("categories").toArray(), (QJsonArray{"trade", "wallet"}));
        QCOMPARE(message[1].toObject().value("include_body").toBool(true), false);
        QCOMPARE(message[2].toObject().value("format").toString(), QString("DEFAULT"));
    }
    void createdAndUpdatedNoticesReplaceByUuidNewestFirst() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(prepare(vault));
        auto socket = std::make_unique<FakeSocket>(); auto* fake = socket.get(); UpbitAnnouncements stream(vault, std::move(socket));
        stream.setActive(true); fake->connect();
        fake->message(announcement("a", QStringLiteral("가상코인(FAKE) 거래지원 종료 안내"), "2026-10-28T09:00:00"));
        fake->message(announcement("b", QStringLiteral("가상코인(SYN) 네트워크 업그레이드에 따른 입출금 일시 중단"), "2026-10-28T10:00:00+09:00", "CREATED", "https://upbit.com/service_center/notice?id=5002", "wallet"));
        auto feed = stream.feed(); QCOMPARE(feed.notices.size(), 2); QCOMPARE(feed.source, OfficialNoticeSource::Upbit);
        QVERIFY(feed.notices[0].title.contains("SYN")); QCOMPARE(feed.notices[0].publishedAtMs, QDateTime::fromString("2026-10-28T01:00:00Z", Qt::ISODate).toMSecsSinceEpoch());
        QCOMPARE(feed.notices[1].publishedAtMs, QDateTime::fromString("2026-10-28T00:00:00Z", Qt::ISODate).toMSecsSinceEpoch()); // Offset-free listed_at is KST.
        fake->message(announcement("a", QStringLiteral("가상코인(FAKE) 거래지원 종료 일정 변경"), "2026-10-28T11:00:00", "UPDATED"));
        feed = stream.feed(); QCOMPARE(feed.notices.size(), 2); QVERIFY(feed.notices[0].title.contains(QStringLiteral("일정 변경")));
        // Wrong host, javascript link, unknown event or category, and keep-alive replies never become notices.
        fake->message(announcement("c", "bad", "2026-10-28T12:00:00", "CREATED", "https://evil.example/service_center/notice?id=1"));
        fake->message(announcement("d", "bad", "2026-10-28T12:00:00", "DELETED"));
        fake->message(announcement("e", "bad", "2026-10-28T12:00:00", "CREATED", "https://upbit.com/service_center/notice?id=1", "airdrop"));
        fake->message(announcement("f", "bad", "yesterday"));
        fake->message(announcement("g", "bad", "2026-10-28T12:00:00", "CREATED", "https://upbit.com/service_center/notice?id=1&id=2"));
        fake->message(announcement("h", "bad", "2026-10-28T12:00:00", "CREATED", "https://upbit.com/service_center/notice?view=share"));
        fake->message(announcement("i", "bad", "2026-10-28T12:00:00", "CREATED", "http://upbit.com/service_center/notice?id=1"));
        emit fake->received("{\"status\":\"UP\"}"); emit fake->received("not json");
        QCOMPARE(stream.feed().notices.size(), 2);
        for (int i = 0; i < UpbitAnnouncements::MaximumNotices + 5; ++i)
            fake->message(announcement(QString("n%1").arg(i), QString("notice %1").arg(i), QString("2026-10-29T%1:00:00").arg(i % 24, 2, 10, QChar('0'))));
        QCOMPARE(stream.feed().notices.size(), UpbitAnnouncements::MaximumNotices);
    }
    void documentedShareLinkKeepsOnlyTheArticleId() {
        // Upbit's documented example adds `&view=share`; the stored link keeps only the numeric id.
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(prepare(vault));
        auto socket = std::make_unique<FakeSocket>(); auto* fake = socket.get(); UpbitAnnouncements stream(vault, std::move(socket));
        stream.setActive(true); fake->connect();
        fake->message(announcement("a", QStringLiteral("가상코인(FAKE) 입출금 일시 중단"), "2026-07-14T16:36:34+09:00", "CREATED",
                                   "https://www.upbit.com/service_center/notice?id=562593104&view=share", "wallet"));
        const auto feed = stream.feed(); QCOMPARE(feed.notices.size(), 1);
        QCOMPARE(feed.notices.first().url, QString("https://www.upbit.com/service_center/notice?id=562593104"));
        QVERIFY(OfficialNotices::allowedArticle(OfficialNoticeSource::Upbit, QUrl(feed.notices.first().url)));
    }
    void removingTheUpbitConnectionClearsItsNotices() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(prepare(vault));
        auto socket = std::make_unique<FakeSocket>(); auto* fake = socket.get(); UpbitAnnouncements stream(vault, std::move(socket));
        stream.setActive(true); fake->connect(); fake->message(announcement("a", QStringLiteral("가상코인(FAKE) 거래유의 지정"), "2026-10-28T09:00:00"));
        QCOMPARE(stream.feed().notices.size(), 1);
        const auto accounts = vault.accounts();
        const auto upbit = std::find_if(accounts.cbegin(), accounts.cend(), [](const auto& a) { return a.provider == "upbit"; });
        QVERIFY(upbit != accounts.cend()); const auto id = upbit->id;
        // New keys on the same Upbit connection: notices stay and the socket reopens with a fresh signature.
        QVERIFY(vault.replaceCredentials(id, {{"access_key", "FakeUpbitAccess456"}, {"secret", "FakeUpbitSecret654321"}}));
        QCOMPARE(stream.feed().notices.size(), 1); QCOMPARE(fake->opened.size(), 2);
        QVERIFY(vault.removeAccount(id));
        QVERIFY(stream.feed().notices.isEmpty()); QVERIFY(!stream.feed().available); QCOMPARE(fake->opened.size(), 2);
    }
    void lockedVaultOrMissingUpbitAccountNeverOpensAndLockClears() {
        QTemporaryDir dir; Vault other(dir.filePath("other")); QVERIFY(other.create(Password)); // Unlocked, no Upbit connection.
        auto noUpbit = std::make_unique<FakeSocket>(); auto* idle = noUpbit.get(); UpbitAnnouncements none(other, std::move(noUpbit));
        none.setActive(true); QVERIFY(idle->opened.isEmpty()); QVERIFY(!none.feed().available); QVERIFY(!none.feed().reason.isEmpty());
        Vault vault(dir.filePath("vault")); QVERIFY(prepare(vault)); vault.lock();
        auto socket = std::make_unique<FakeSocket>(); auto* fake = socket.get(); UpbitAnnouncements stream(vault, std::move(socket));
        stream.setActive(true); QVERIFY(fake->opened.isEmpty());
        QVERIFY(vault.unlock(Password)); QCOMPARE(fake->opened.size(), 1); // Unlock while active connects without another view event.
        fake->connect(); fake->message(announcement("a", QStringLiteral("가상코인(FAKE) 거래유의 지정"), "2026-10-28T09:00:00"));
        QCOMPARE(stream.feed().notices.size(), 1);
        const auto aborts = fake->aborts; vault.lock();
        QVERIFY(fake->aborts > aborts); QVERIFY(!stream.feed().available); QVERIFY(stream.feed().notices.isEmpty());
        fake->connect(); QVERIFY(fake->sent.size() == 1); // A late connect after lock never resubscribes.
    }
    void dropReconnectsLaterWithAFreshSignature() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); QVERIFY(prepare(vault));
        auto socket = std::make_unique<FakeSocket>(); auto* fake = socket.get(); UpbitAnnouncements stream(vault, std::move(socket));
        stream.setActive(true); fake->connect(); fake->message(announcement("a", "kept", "2026-10-28T09:00:00"));
        fake->drop(); QVERIFY(!stream.feed().available); QCOMPARE(stream.feed().notices.size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(fake->opened.size(), 2, 7000);
        QVERIFY(fake->opened[0].rawHeader("Authorization") != fake->opened[1].rawHeader("Authorization"));
        stream.setActive(false); fake->drop(); QTest::qWait(50); QCOMPARE(fake->opened.size(), 2);
    }
};
QTEST_GUILESS_MAIN(UpbitAnnouncementsTest)
#include "tst_upbit_announcements.moc"
