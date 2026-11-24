#include "TooltipTestSupport.h"
#include "PublicViewPreview.h"
#include "keefi/OfficialNotices.h"
#include "keefi/OfficialNoticesView.h"
#include "keefi/ExploreDashboard.h"
#include "keefi/TradingTransport.h"
#include "keefi/UpbitAnnouncements.h"
#include "keefi/AccountModel.h"
#include "keefi/Vault.h"
#include <QTemporaryDir>
#include "keefi/InfoButton.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include <QDateTime>
#include <QFile>
#include <QFontMetrics>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QTest>
#include <tuple>
#include <QDialog>
#include <QPlainTextEdit>
#include <QTextDocument>
#include <QDesktopServices>
#include <QToolButton>
#include <QSettings>
#include <QTimer>
using namespace keefi;
namespace {
QString visibleText(const QLabel* label){
    if(label->textFormat()==Qt::PlainText||!Qt::mightBeRichText(label->text()))return label->text();
    QTextDocument document;document.setHtml(label->text());return document.toPlainText();
}
qint64 at(const QString& value) { return QDateTime::fromString(value, Qt::ISODate).toMSecsSinceEpoch(); }
const qint64 Now = at("2026-10-22T10:00:00Z");
QJsonObject okxRow(const QString& suffix = "one") {
    return {{"annType", "announcements-delisting"}, {"title", "OKX to delist FAKEUSDT perpetual futures"},
        {"url", "https://www.okx.com/help/" + suffix}, {"pTime", QString::number(Now - 60000)},
        {"businessPTime", QString::number(Now - 60000)}};
}
QByteArray okxFeed(const QJsonArray& rows) {
    return QJsonDocument(QJsonObject{{"code", "0"}, {"msg", ""},
        {"data", QJsonArray{QJsonObject{{"details", rows}, {"totalPage", "12"}}}}}).toJson(QJsonDocument::Compact);
}
QJsonObject bithumbRow() {
    return {{"categories", QJsonArray{"거래유의"}}, {"title", "가상코인(FAKE) 거래유의 지정"},
        {"pc_url", "https://feed.bithumb.com/notice/12345"}, {"published_at", "2026-10-22 09:00:00"},
        {"modified_at", "2026-10-22 09:01:00"}};
}
class UpbitFakeSocket final : public UpbitAnnouncementSocket {
  public:
    int opens = 0;
    void open(const QNetworkRequest&) override { ++opens; }
    void send(const QByteArray&) override {}
    void abort() override {}
    void connect() { emit connected(); }
    void message(const QByteArray& json) { emit received(json); }
};
class NoticesTransport final : public TradingTransport {
  public:
    QList<RequestSpec> sent; quint64 token = 0; int aborts = 0;
    void send(quint64 next, const RequestSpec& r) override { token = next; sent.append(r); }
    void abort() override { ++aborts; }
    void answer(int status, const QByteArray& body = {}) { HttpResult r; r.status = status; r.body = body; emit finished(token, r); }
};
}
class NoticeUrlCapture final : public QObject {
    Q_OBJECT
  public:
    QList<QUrl> opened;
  public slots:
    void capture(const QUrl& url){opened.append(url);}
};
class OfficialNoticesTest final : public QObject {
    Q_OBJECT
    QTemporaryDir settingsRoot_;
  private slots:
    void initTestCase(){
        QVERIFY(settingsRoot_.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settingsRoot_.filePath("user"));
        QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,settingsRoot_.filePath("system"));
    }
    void cleanup(){KeefiTheme::instance()->setMode(ThemeMode::System);}
    void todayNoticeTimesUseKstAcrossMidnight_data(){
        QTest::addColumn<QString>("utc");QTest::addColumn<QString>("observedUtc");
        QTest::addColumn<QString>("shown");QTest::addColumn<QString>("details");QTest::addColumn<bool>("otherSourceFails");
        QTest::newRow("before-kst-midnight")<<QString("2026-11-06T14:59:00Z")<<QString("2026-11-06T14:59:30Z")
            <<QString("게시 11/06 23:59 KST · UTC+9")<<QString("최초 게시 2026-11-06 23:59 KST · UTC+9")<<false;
        QTest::newRow("after-kst-midnight")<<QString("2026-11-06T15:01:00Z")<<QString("2026-11-06T15:02:00Z")
            <<QString("게시 11/07 00:01 KST · UTC+9")<<QString("최초 게시 2026-11-07 00:01 KST · UTC+9")<<false;
        QTest::newRow("yesterday-published-today-received")<<QString("2026-11-06T01:10:00Z")<<QString("2026-11-07T00:10:00Z")
            <<QString("게시 11/06 10:10 KST · UTC+9")<<QString("최초 게시 2026-11-06 10:10 KST · UTC+9")<<false;
        QTest::newRow("partial-source-failure-keeps-publication")<<QString("2026-11-06T01:10:00Z")<<QString("2026-11-07T00:10:00Z")
            <<QString("게시 11/06 10:10 KST · UTC+9")<<QString("최초 게시 2026-11-06 10:10 KST · UTC+9")<<true;
    }
    void todayNoticeTimesUseKstAcrossMidnight(){
        QFETCH(QString,utc);QFETCH(QString,observedUtc);QFETCH(QString,shown);QFETCH(QString,details);QFETCH(bool,otherSourceFails);
        const auto posted=at(utc),received=at(observedUtc);NoticesTransport transport;
        OfficialNotices model(&transport,nullptr,[=]{return received;});
        ExploreTodayView view;view.resize(1000,800);view.show();
        connect(&model,&OfficialNotices::changed,&view,[&]{view.setNoticeFeeds({model.feed(OfficialNoticeSource::Okx),model.feed(OfficialNoticeSource::Bithumb)});});
        model.setActive(true);QTRY_COMPARE(transport.sent.size(),1);
        auto row=okxRow();row["pTime"]=QString::number(posted);row["businessPTime"]=QString::number(posted);
        QTimer::singleShot(0,&transport,[&]{transport.answer(200,okxFeed({row}));});
        QTRY_COMPARE(transport.sent.size(),2);
        QTimer::singleShot(0,&transport,[&]{transport.answer(otherSourceFails?503:200,"[]");});
        QTRY_VERIFY(!model.feed(OfficialNoticeSource::Bithumb).loading);
        QCOMPARE(model.feed(OfficialNoticeSource::Bithumb).available,!otherSourceFails);
        QCOMPARE(model.feed(OfficialNoticeSource::Okx).receivedAtMs,received);
        QCOMPARE(model.feed(OfficialNoticeSource::Okx).notices.first().publishedAtMs,posted);
        auto* panel=view.findChild<QWidget*>("todayNoticePanel");QVERIFY(panel);
        const auto visibleTime=[&]{for(auto* label:panel->findChildren<QLabel*>())if(label->isVisible()&&label->text().contains(shown))return true;return false;};
        QTRY_VERIFY2(visibleTime(),qPrintable("Today must label its displayed notice time: "+shown));
        if(otherSourceFails){bool warning=false;for(auto* label:panel->findChildren<QLabel*>())
            warning|=label->isVisible()&&label->text().contains("일부 거래소 공지를 받지 못했어요");QVERIFY(warning);}
        auto* info=panel->findChild<InfoButton*>();QVERIFY(info);QTest::mouseClick(info,Qt::LeftButton);
        QTRY_VERIFY(info->informationOpen());
        auto* text=info->findChild<QPlainTextEdit*>("infoPublicText");QVERIFY(text);
        QVERIFY2(text->toPlainText().contains(details),qPrintable(text->toPlainText()));
        model.setActive(false);
    }
    void todayNoticeDoesNotInventTimezoneForSourceLocalText_data(){
        QTest::addColumn<bool>("otherSourceFails");QTest::newRow("both-available")<<false;QTest::newRow("okx-fails-bithumb-kept")<<true;
    }
    void todayNoticeDoesNotInventTimezoneForSourceLocalText(){
        QFETCH(bool,otherSourceFails);NoticesTransport transport;OfficialNotices model(&transport,nullptr,[]{return Now;});
        ExploreTodayView view;view.resize(1000,800);view.show();
        connect(&model,&OfficialNotices::changed,&view,[&]{view.setNoticeFeeds({model.feed(OfficialNoticeSource::Okx),model.feed(OfficialNoticeSource::Bithumb)});});
        model.setActive(true);QTRY_COMPARE(transport.sent.size(),1);
        QTimer::singleShot(0,&transport,[&]{transport.answer(otherSourceFails?503:200,okxFeed({}));});
        QTRY_COMPARE(transport.sent.size(),2);
        QTimer::singleShot(0,&transport,[&]{transport.answer(200,QJsonDocument(QJsonArray{bithumbRow()}).toJson());});
        QTRY_VERIFY(model.feed(OfficialNoticeSource::Bithumb).available);
        QCOMPARE(model.feed(OfficialNoticeSource::Bithumb).notices.first().publishedAtMs,0);
        auto* panel=view.findChild<QWidget*>("todayNoticePanel");QVERIFY(panel);
        const auto rawPublicationVisible=[&]{for(auto* label:panel->findChildren<QLabel*>())if(label->isVisible()&&
            label->text()==QString("게시 2026-10-22 09:00:00 · 시간대 미제공"))return true;return false;};
        QTRY_VERIFY_WITH_TIMEOUT(rawPublicationVisible(),1000);
        if(otherSourceFails){bool warning=false;for(auto* label:panel->findChildren<QLabel*>())
            warning|=label->isVisible()&&label->text().contains("일부 거래소 공지를 받지 못했어요");QVERIFY(warning);}
        auto* info=panel->findChild<InfoButton*>();QVERIFY(info);QTRY_VERIFY(info->isVisible());QTest::mouseClick(info,Qt::LeftButton);
        QTRY_VERIFY(info->informationOpen());
        auto* text=info->findChild<QPlainTextEdit*>("infoPublicText");QVERIFY(text);
        QVERIFY(text->toPlainText().contains("2026-10-22 09:00:00"));
        QVERIFY(text->toPlainText().contains("시간대 미제공"));
        QVERIFY(!text->toPlainText().contains("KST"));
        model.setActive(false);
    }
    void todayThreeNoticeRowsRemainReadableAtNarrowWidths_data(){
        QTest::addColumn<QSize>("size");QTest::addColumn<bool>("rawTime");
        QTest::newRow("720x900-absolute")<<QSize(720,900)<<false;QTest::newRow("942x800-absolute")<<QSize(942,800)<<false;
        QTest::newRow("720x900-source-local")<<QSize(720,900)<<true;QTest::newRow("942x800-source-local")<<QSize(942,800)<<true;
    }
    void todayThreeNoticeRowsRemainReadableAtNarrowWidths(){
        QFETCH(QSize,size);QFETCH(bool,rawTime);QVERIFY(KeefiFonts::registerBundledFonts());
        NoticesTransport transport;OfficialNotices model(&transport,nullptr,[]{return Now;});
        ExploreTodayView view;view.setStyleSheet(KeefiTheme::localStyleSheet(&view));view.setFixedSize(size);view.show();
        connect(&model,&OfficialNotices::changed,&view,[&]{view.setNoticeFeeds({model.feed(OfficialNoticeSource::Okx),model.feed(OfficialNoticeSource::Bithumb)});});
        model.setActive(true);QTRY_COMPARE(transport.sent.size(),1);
        transport.answer(200,okxFeed(rawTime?QJsonArray{}:QJsonArray{okxRow("one"),okxRow("two"),okxRow("three")}));
        QTRY_COMPARE(transport.sent.size(),2);
        QJsonArray local;for(int i=0;i<3&&rawTime;++i){auto row=bithumbRow();row["pc_url"]=QString("https://feed.bithumb.com/notice/%1").arg(12345+i);local.append(row);}
        transport.answer(200,QJsonDocument(local).toJson());
        auto* panel=view.findChild<QWidget*>("todayNoticePanel");QVERIFY(panel);
        QTRY_COMPARE(panel->findChildren<InfoButton*>().size(),3);QCoreApplication::processEvents();
        const auto* top=panel->parentWidget();QVERIFY(top);
        QVERIFY(top->rect().contains(QRect(panel->mapTo(top,QPoint()),panel->size())));
        int dates=0;
        for(auto* label:panel->findChildren<QLabel*>()){
            if(!label->isVisible()||label->text().isEmpty())continue;
            if(label->text().contains("게시 "))++dates;
            const QRect bounds(label->mapTo(panel,QPoint()),label->size());
            QVERIFY2(panel->rect().contains(bounds),qPrintable(QString("Label escapes the notice panel: %1, bounds %2,%3 %4x%5, panel %6x%7")
                .arg(label->text()).arg(bounds.x()).arg(bounds.y()).arg(bounds.width()).arg(bounds.height()).arg(panel->width()).arg(panel->height())));
            const auto contents=label->contentsRect();
            const auto flags=label->wordWrap()?Qt::TextWordWrap:Qt::TextSingleLine;
            const auto needed=label->fontMetrics().boundingRect(QRect(0,0,contents.width(),10000),flags,label->text());
            QVERIFY2(needed.width()<=contents.width()&&needed.height()<=contents.height(),
                qPrintable(QString("Clipped notice text: %1; needs %2x%3, has %4x%5")
                    .arg(label->text()).arg(needed.width()).arg(needed.height()).arg(contents.width()).arg(contents.height())));
        }
        QCOMPARE(dates,3);model.setActive(false);
    }
    void receivedNoticeTitleStaysLiteralInItsInformationTooltip() {
        NoticesTransport transport;
        OfficialNotices model(&transport, nullptr, [] { return Now; });
        OfficialNoticesView view(&model);
        view.show();
        QTRY_COMPARE(transport.sent.size(), 1);
        const auto title = QStringLiteral("<b>합성 공지 제목</b> & literal");
        auto row = okxRow();
        row["title"] = title;
        transport.answer(200, okxFeed({row}));
        QTRY_COMPARE(transport.sent.size(), 2);
        transport.answer(200, QJsonDocument(QJsonArray{}).toJson());
        auto* information = view.findChild<InfoButton*>("officialNoticeInfo");
        QVERIFY(information);
        QCOMPARE(model.feed(OfficialNoticeSource::Okx).notices.first().title, title);
        QCOMPARE(information->accessibleName(), title + QStringLiteral(" 정보 보기"));
        const auto shown = keefi::test::projectTooltip(information->toolTip());
        QCOMPARE(shown.text, information->accessibleName());
        QVERIFY(!shown.image);
        QVERIFY(!shown.anchor);
        QCOMPARE(shown.resourceRequests, 0);
    }
    void noticeDisplayHidesUrlsAndKeepsOriginalArticleAction(){
        NoticesTransport transport;OfficialNotices model(&transport,nullptr,[]{return Now;});
        OfficialNoticesView view(&model);view.resize(900,700);view.show();
        QTRY_COMPARE(transport.sent.size(),1);transport.answer(200,okxFeed({okxRow()}));
        QTRY_COMPARE(transport.sent.size(),2);transport.answer(200,QJsonDocument(QJsonArray{bithumbRow()}).toJson());
        const auto original=model.feed(OfficialNoticeSource::Okx).notices.first();
        QCOMPARE(original.url,QString("https://www.okx.com/help/one"));
        QCOMPARE(original.publishedAtMs,Now-60000);
        NoticeUrlCapture receiver;QDesktopServices::setUrlHandler("https",&receiver,"capture");
        struct ResetUrlHandler{~ResetUrlHandler(){QDesktopServices::unsetUrlHandler("https");}} reset;
        auto* row=view.findChild<QWidget*>("officialNoticeRow");QVERIFY(row);
        QToolButton* open=nullptr;for(auto* button:row->findChildren<QToolButton*>())if(!qobject_cast<InfoButton*>(button))open=button;
        QVERIFY(open);open->click();QCOMPARE(receiver.opened,QList<QUrl>{QUrl(original.url)});
        QVERIFY(open->toolTip().contains("OKX"));QVERIFY(open->toolTip().contains("18:59"));QVERIFY(!open->toolTip().contains('\n'));
        for(auto* widget:view.findChildren<QWidget*>()){
            QVERIFY2(!widget->toolTip().contains("://"),qPrintable(widget->toolTip()));
            QVERIFY(!widget->accessibleName().contains("://"));QVERIFY(!widget->accessibleDescription().contains("://"));
        }
        for(auto* info:view.findChildren<InfoButton*>()){
            info->openInformation();QTRY_VERIFY(info->informationOpen());
            auto* dialog=info->findChild<QDialog*>("infoDialog");QVERIFY(dialog);
            const auto text=dialog->findChild<QPlainTextEdit*>("infoPublicText")->toPlainText();
            QVERIFY2(!text.contains("://"),qPrintable(text));dialog->reject();
        }
        QCOMPARE(model.feed(OfficialNoticeSource::Okx).notices.first().url,original.url);
        QCOMPARE(model.feed(OfficialNoticeSource::Okx).notices.first().publishedAtMs,original.publishedAtMs);
        QCOMPARE(transport.sent.size(),2);
    }
    void noticeTypographyKeepsPublicEvidenceAndPlainTitles_data(){
        QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;
    }
    void noticeTypographyKeepsPublicEvidenceAndPlainTitles(){
        QFETCH(bool,dark);KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
        NoticesTransport transport;OfficialNotices model(&transport,nullptr,[]{return Now;});OfficialNoticesView view(&model);view.resize(980,500);view.show();
        QTRY_COMPARE(transport.sent.size(),1);transport.answer(200,okxFeed({okxRow()}));QTRY_COMPARE(transport.sent.size(),2);
        transport.answer(200,QJsonDocument(QJsonArray{bithumbRow()}).toJson());
        auto* title=view.findChild<QLabel*>("officialNoticesTitle");auto* status=view.findChild<QLabel*>("officialNoticesStatus");
        QVERIFY(title);QVERIFY(status);QCOMPARE(title->font(),KeefiFonts::font(FontRole::Title17));
        QCOMPARE(status->font(),KeefiFonts::font(FontRole::Caption12));
        const auto statusText=visibleText(status);QVERIFY(statusText.contains(QStringLiteral("10/22 19:00 KST · UTC+9")));
        const auto rows=view.findChildren<QWidget*>("officialNoticeRow");QCOMPARE(rows.size(),2);
        for(auto* row:rows){auto* content=row->findChild<QLabel*>("officialNoticeTitle");QVERIFY(content);
            QCOMPARE(content->font(),KeefiFonts::font(FontRole::Body15));QCOMPARE(visibleText(content),content->accessibleName());}
        const auto renderPath=qEnvironmentVariable("KEEFI_D5_RENDER_DIR");if(!renderPath.isEmpty()){
            QVERIFY(QDir().mkpath(renderPath));view.setStyleSheet(KeefiTheme::localStyleSheet(&view));QCoreApplication::processEvents();
            QVERIFY(view.grab().save(QDir(renderPath).filePath(QStringLiteral("official-notices-%1.png").arg(dark?"dark":"light"))));
        }
        const auto original=model.feed(OfficialNoticeSource::Bithumb).notices.first();QCOMPARE(original.publishedAtMs,qint64(0));
        KeefiTheme::instance()->setMode(dark?ThemeMode::Light:ThemeMode::Dark);QCoreApplication::processEvents();
        QCOMPARE(visibleText(status),statusText);QCOMPARE(model.feed(OfficialNoticeSource::Bithumb).notices.first().publishedText,original.publishedText);
        QCOMPARE(transport.sent.size(),2);QCOMPARE(status->palette().color(QPalette::WindowText),themeColors().text3);
    }
    void compactNoticeRowExposesOriginalTimezoneFreeEvidenceFromKeyboardAndMouse(){
        NoticesTransport transport;OfficialNotices model(&transport,nullptr,[]{return Now;});OfficialNoticesView view(&model);view.resize(900,700);view.show();QTRY_COMPARE(transport.sent.size(),1);
        transport.answer(200,okxFeed({}));QTRY_COMPARE(transport.sent.size(),2);transport.answer(200,QJsonDocument(QJsonArray{bithumbRow()}).toJson());QVERIFY(model.feed(OfficialNoticeSource::Bithumb).available);QCOMPARE(model.feed(OfficialNoticeSource::Bithumb).notices.first().publishedAtMs,qint64(0));
        auto* row=view.findChild<QWidget*>("officialNoticeRow");QVERIFY(row);auto* title=row->findChild<QLabel*>("officialNoticeTitle");QVERIFY(title);QCOMPARE(visibleText(title),QStringLiteral("가상코인(FAKE) 거래유의 지정"));QVERIFY(!title->wordWrap());
        auto* info=row->findChild<InfoButton*>("officialNoticeInfo");QVERIFY(info);QVERIFY(!info->accessibleName().isEmpty());info->setFocus();QTest::keyClick(info,Qt::Key_Space);QTRY_VERIFY(info->informationOpen());auto* dialog=info->findChild<QDialog*>("infoDialog");QVERIFY(dialog);auto* text=dialog->findChild<QPlainTextEdit*>("infoPublicText");QVERIFY(text);
        QVERIFY(text->toPlainText().contains("2026-10-22 09:00:00"));QVERIFY(text->toPlainText().contains(QStringLiteral("시간대 미제공")));QVERIFY(!text->toPlainText().contains("://"));QVERIFY(text->toPlainText().contains("2026-10-22 09:01:00"));
        QCOMPARE(model.feed(OfficialNoticeSource::Bithumb).notices.first().url,QString("https://feed.bithumb.com/notice/12345"));
        QTest::keyClick(dialog,Qt::Key_Escape);QTRY_VERIFY(!info->informationOpen());QTest::mouseClick(info,Qt::LeftButton);QTRY_VERIFY(info->informationOpen());QTest::keyClick(info->findChild<QDialog*>("infoDialog"),Qt::Key_Escape);QCOMPARE(transport.sent.size(),2);
        auto* header=view.findChild<InfoButton*>("officialNoticesInfo");QVERIFY(header);QTest::mouseClick(header,Qt::LeftButton);QTRY_VERIFY(header->informationOpen());auto* headerDialog=header->findChild<QDialog*>("infoDialog");QVERIFY(headerDialog);auto* coverage=headerDialog->findChild<QPlainTextEdit*>("infoPublicText");QVERIFY(coverage);QVERIFY(coverage->toPlainText().contains(QStringLiteral("자동 보유 매칭 미지원")));QVERIFY(coverage->toPlainText().contains(QStringLiteral("위험 없음")));QTest::keyClick(headerDialog,Qt::Key_Escape);QCoreApplication::processEvents();QVERIFY(row->height()<=60);QVERIFY(row->geometry().top()<=view.findChild<QLabel*>("officialNoticesStatus")->geometry().bottom()+24);QVERIFY(keefi::test::savePublicViewPreview(view,"c9-official-notices"));
    }
    void upbitRealtimeNoticeJoinsTheFlowListWithItsOwnArticleRule() {
        QTemporaryDir dir; Vault vault(dir.filePath("vault")); AccountRecord account; account.provider = "upbit"; account.market = "spot"; account.environment = "live"; account.alias = "fake"; account.currency = "KRW";
        QVERIFY(vault.create("Fake Notice Flow Password! 2026") && vault.addAccount(account, {{"access_key", "FakeUpbitAccess123"}, {"secret", "FakeUpbitSecret123456"}}, StorageMode::EncryptedVault));
        auto socket = std::make_unique<UpbitFakeSocket>(); auto* fake = socket.get(); UpbitAnnouncements upbit(vault, std::move(socket));
        NoticesTransport transport; OfficialNotices model(&transport, nullptr, [] { return Now; }); model.setUpbitAnnouncements(&upbit);
        QCOMPARE(fake->opens, 0); // Inactive until the notices view is shown.
        OfficialNoticesView view(&model); view.setMarketFlowMode(true); view.resize(700, 500); view.show();
        QTRY_COMPARE(fake->opens, 1); QTRY_COMPARE(transport.sent.size(), 1); transport.answer(403); QTRY_COMPARE(transport.sent.size(), 2); transport.answer(403);
        fake->connect(); QVERIFY(model.feed(OfficialNoticeSource::Upbit).available);
        fake->message(QJsonDocument(QJsonObject{{"type", "announcement"}, {"event_type", "CREATED"}, {"uuid", "u1"}, {"title", QStringLiteral("가상코인(FAKE) 거래지원 종료 안내")},
            {"category", "trade"}, {"url", "https://upbit.com/service_center/notice?id=5001"}, {"first_listed_at", "2026-10-22T18:00:00"},
            {"listed_at", "2026-10-22T18:00:00"}, {"timestamp", 1}, {"stream_type", "REALTIME"}}).toJson(QJsonDocument::Compact));
        QTRY_VERIFY(view.findChild<QWidget*>("officialNoticeRow"));
        QStringList texts; for (auto* label : view.findChildren<QLabel*>()) texts << visibleText(label);
        QVERIFY2(texts.join('\n').contains(QStringLiteral("가상코인(FAKE) 거래지원 종료 안내")), qPrintable(texts.join('|')));
        QVERIFY(texts.join('\n').contains(QStringLiteral("업비트")));
        QVERIFY(OfficialNotices::allowedArticle(OfficialNoticeSource::Upbit, QUrl("https://upbit.com/service_center/notice?id=5001")));
        QVERIFY(!OfficialNotices::allowedArticle(OfficialNoticeSource::Upbit, QUrl("https://upbit.com/service_center/notice?id=5001&next=https://evil.example")));
        QVERIFY(!OfficialNotices::allowedArticle(OfficialNoticeSource::Bithumb, QUrl("https://upbit.com/service_center/notice?id=5001")));
        view.hide(); QVERIFY(!model.feed(OfficialNoticeSource::Upbit).available);
    }
    void exactPublicGetOnly() {
        for (auto source : {OfficialNoticeSource::Okx, OfficialNoticeSource::Bithumb}) {
            const auto r = OfficialNotices::request(source);
            QVERIFY(OfficialNotices::isRequest(r)); QVERIFY(NetworkTradingTransport::allowed(r));
            QCOMPARE(r.method, QByteArray("GET")); QVERIFY(!r.authenticated); QVERIFY(r.body.isEmpty());
            for (int fault = 0; fault < 8; ++fault) {
                auto bad = r;
                if (fault == 0) bad.method = "POST";
                if (fault == 1) bad.authenticated = true;
                if (fault == 2) bad.headers.insert("Authorization", "private");
                if (fault == 3) bad.url.setQuery("asset=PRIVATE_HOLDING");
                if (fault == 4) bad.url.setHost("example.test");
                if (fault == 5) bad.body = "{}";
                if (fault == 6) bad.url.setPort(444);
                if (fault == 7) bad.headers.insert("OK-ACCESS-KEY", "private");
                QVERIFY(!OfficialNotices::isRequest(bad)); QVERIFY(!NetworkTradingTransport::allowed(bad));
            }
        }
    }
    void parsesOfficialListsWithoutClaimingHoldingMatches() {
        OfficialNoticeFeed feed;
        QVERIFY(OfficialNotices::parse(OfficialNoticeSource::Okx, okxFeed({okxRow()}), Now, &feed));
        QVERIFY(feed.available); QCOMPARE(feed.notices.size(), 1); QCOMPARE(feed.receivedAtMs, Now);
        QCOMPARE(feed.notices[0].publishedAtMs, Now - 60000);
        QVERIFY(!OfficialNotices::matchingUnavailableReason("okx", "spot").isEmpty());
        QVERIFY(!OfficialNotices::matchingUnavailableReason("phantom", "solana").isEmpty());
    }
    void viewNamesPublicListAndMatchingLimit() {
        OfficialNotices model(nullptr); OfficialNoticesView view(&model);
        auto* title = view.findChild<QLabel*>("officialNoticesTitle"); QVERIFY(title);
        QVERIFY(visibleText(title).contains("거래소 공식 공지"));
        auto* info=view.findChild<InfoButton*>("officialNoticesInfo");QVERIFY(info);view.show();QTest::mouseClick(info,Qt::LeftButton);QTRY_VERIFY(info->informationOpen());
        auto* dialog=info->findChild<QDialog*>("infoDialog");QVERIFY(dialog);const auto description=dialog->findChild<QPlainTextEdit*>("infoPublicText")->toPlainText();
        QVERIFY(description.contains("자동 보유 매칭 미지원"));QVERIFY(description.contains("위험 없음"));QTest::keyClick(dialog,Qt::Key_Escape);
    }
    void timezoneFreeSourceTimeIsNotInvented() {
        OfficialNoticeFeed feed;
        QVERIFY(OfficialNotices::parse(OfficialNoticeSource::Bithumb, QJsonDocument(QJsonArray{bithumbRow()}).toJson(), Now, &feed));
        QCOMPARE(feed.notices.size(), 1); QCOMPARE(feed.notices[0].publishedAtMs, qint64(0));
        QCOMPARE(feed.notices[0].publishedText, QString("2026-10-22 09:00:00"));
        QCOMPARE(feed.notices[0].modifiedText, QString("2026-10-22 09:01:00"));
        auto bad = bithumbRow(); bad["published_at"] = "2026-02-30 09:00:00";
        QVERIFY(!OfficialNotices::parse(OfficialNoticeSource::Bithumb, QJsonDocument(QJsonArray{bad}).toJson(), Now, &feed));
        QVERIFY(feed.notices.isEmpty()); QVERIFY(!feed.available);
    }
    void malformedOrUntrustedRowsClearEntireFeed_data() {
        QTest::addColumn<QByteArray>("input");
        QTest::newRow("html-redirect") << QByteArray("<html>302 Found</html>");
        QTest::newRow("source-error") << QByteArray(R"({"code":"50011","data":[],"msg":"rate limit"})");
        QTest::newRow("no-data") << QByteArray(R"({"code":"0","data":[]})");
        for (const auto& [name, field, value] : QList<std::tuple<const char*, QString, QString>>{
            {"future", "pTime", QString::number(Now + 1)}, {"invalid-time", "pTime", "not-time"},
            {"empty-title", "title", ""}, {"wrong-origin", "url", "https://www.okx.com.evil.test/help/one"},
            {"userinfo", "url", "https://private@www.okx.com/help/one"},
            {"query", "url", "https://www.okx.com/help/one?redirect=evil"},
            {"fragment", "url", "https://www.okx.com/help/one#evil"},
            {"traversal", "url", "https://www.okx.com/help/%2e%2e/account"},
            {"other-path", "url", "https://www.okx.com/trade-spot/btc-usdt"}}) {
            auto row = okxRow(); row[field] = value; QTest::newRow(name) << okxFeed({row});
        }
        QTest::newRow("duplicate") << okxFeed({okxRow(), okxRow()});
        QJsonArray tooMany; for (int i = 0; i < 21; ++i) tooMany.append(okxRow(QString::number(i)));
        QTest::newRow("too-many") << okxFeed(tooMany);
        QTest::newRow("oversize") << QByteArray(512 * 1024 + 1, 'x');
    }
    void malformedOrUntrustedRowsClearEntireFeed() {
        QFETCH(QByteArray, input);
        OfficialNoticeFeed feed; feed.available = true; feed.notices.append(OfficialNotice{"old", "old"});
        QVERIFY(!OfficialNotices::parse(OfficialNoticeSource::Okx, input, Now, &feed));
        QVERIFY(!feed.available); QVERIFY(feed.notices.isEmpty()); QVERIFY(!feed.reason.isEmpty());
    }
    void unsupportedIdentityReasonsNeverPromoteTickerNames() {
        QVERIFY(OfficialNotices::matchingUnavailableReason("phantom", "solana").contains("Mint"));
        QVERIFY(OfficialNotices::matchingUnavailableReason("binance", "spot").contains("연결 미지원"));
        QVERIFY(OfficialNotices::matchingUnavailableReason("upbit", "spot").contains("연결 미지원"));
        QVERIFY(OfficialNotices::matchingUnavailableReason("okx", "spot").contains("식별자"));
        QVERIFY(OfficialNotices::matchingUnavailableReason("okx", "swap").contains("식별자"));
    }
    void independentSourcesExpireClearAndIgnoreLateCallbacks() {
        NoticesTransport transport; OfficialNotices model(&transport, nullptr, [] { return Now; });
        model.setActive(true); QTRY_COMPARE(transport.sent.size(), 1);
        QCOMPARE(transport.sent[0].url.host(), QString("openapi.okx.com"));
        transport.answer(200, okxFeed({okxRow()})); QTRY_COMPARE(transport.sent.size(), 2);
        transport.answer(302, "<html>302 Found</html>");
        QVERIFY(model.feed(OfficialNoticeSource::Okx).available);
        QVERIFY(!model.feed(OfficialNoticeSource::Bithumb).available);
        QVERIFY(model.feed(OfficialNoticeSource::Bithumb).reason.contains("302"));
        const auto expired = model.feed(OfficialNoticeSource::Okx, Now + OfficialNotices::FreshnessMs + 1);
        QVERIFY(!expired.available); QVERIFY(expired.notices.isEmpty());
        QVERIFY(!model.feed(OfficialNoticeSource::Okx, Now - 1).available);
        model.refresh(); QTRY_COMPARE(transport.sent.size(), 3); QVERIFY(model.feed(OfficialNoticeSource::Okx).notices.isEmpty());
        transport.answer(503); QTRY_COMPARE(transport.sent.size(), 4); const auto oldToken = transport.token;
        model.setActive(false); QVERIFY(transport.aborts > 0);
        HttpResult late; late.status = 200; late.body = QJsonDocument(QJsonArray{bithumbRow()}).toJson();
        emit transport.finished(oldToken, late);
        QVERIFY(model.feed(OfficialNoticeSource::Bithumb).notices.isEmpty());
    }
    void publicRowsStayPlainAndViewLifetimeStopsPolling() {
        NoticesTransport transport; OfficialNotices model(&transport, nullptr, [] { return Now; });
        auto* view = new OfficialNoticesView(&model); view->show(); QTRY_COMPARE(transport.sent.size(), 1);
        auto row = okxRow(); row["title"] = "<b>FAKE</b> symbol is not a holding match";
        transport.answer(200, okxFeed({row})); QTRY_COMPARE(transport.sent.size(), 2); transport.answer(302);
        const auto labels = view->findChildren<QLabel*>(); bool found = false;
        for (auto* label : labels) if (visibleText(label).contains("<b>FAKE</b>")) {
            found = true;QCOMPARE(visibleText(label),QStringLiteral("<b>FAKE</b> symbol is not a holding match"));
            QVERIFY(!label->text().contains("<b>FAKE</b>")); // Source markup is displayed literally, never interpreted.
        }
        QVERIFY(found);
        delete view; QVERIFY(transport.aborts > 0); const auto count = transport.sent.size(); model.refresh();
        QCoreApplication::processEvents(); QCOMPARE(transport.sent.size(), count);
    }
    void publicDownloadedFixture() {
        const auto path = qEnvironmentVariable("KEEFI_PUBLIC_NOTICES_FIXTURE");
        if (path.isEmpty()) QSKIP("Opt-in official public OKX fixture");
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); OfficialNoticeFeed feed;
        QVERIFY(OfficialNotices::parse(OfficialNoticeSource::Okx, file.readAll(), QDateTime::currentMSecsSinceEpoch(), &feed));
        QCOMPARE(feed.notices.size(), 20);
    }
    void publicFeedsOverTransport() {
        if (!qEnvironmentVariableIsSet("KEEFI_PUBLIC_NOTICES_NETWORK")) QSKIP("Opt-in official public GET only");
        NetworkTradingTransport transport; OfficialNotices model(&transport); model.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(!model.feed(OfficialNoticeSource::Okx).loading && !model.feed(OfficialNoticeSource::Bithumb).loading, 45000);
        const auto okx = model.feed(OfficialNoticeSource::Okx); QVERIFY2(okx.available, qPrintable(okx.reason)); QVERIFY(!okx.notices.isEmpty());
        qInfo() << "Public OKX notices:" << okx.notices.size(); qInfo() << "Public Bithumb status:" << model.feed(OfficialNoticeSource::Bithumb).reason;
        model.setActive(false);
    }
};
QTEST_MAIN(OfficialNoticesTest)
#include "tst_official_notices.moc"
