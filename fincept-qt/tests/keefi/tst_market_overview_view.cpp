#include "PublicViewPreview.h"
#include "keefi/MarketOverviewView.h"
#include "keefi/TradingTransport.h"
#include "keefi/ExploreMetricCard.h"
#include "keefi/InfoButton.h"
#include <QComboBox>
#include <QApplication>
#include <QAbstractItemView>
#include <QDateTime>
#include <QLabel>
#include <QPushButton>
#include <QTest>
#include <QTimeZone>
#include <QDialog>
#include <QPlainTextEdit>
#include <QDir>
#include "keefi/KeefiTheme.h"
namespace keefi {
class OverviewViewTransport final:public TradingTransport {
  public:
    quint64 token=0;int requests=0,aborts=0;
    void send(quint64 next,const RequestSpec&)override{token=next;++requests;}
    void abort()override{++aborts;}
    void complete(const HttpResult& response){emit finished(token,response);}
};
class MarketOverviewViewTests final:public QObject {
    Q_OBJECT
  private slots:
    void optionalSyntheticMarketOverviewRenders(){
        const auto directory=qEnvironmentVariable("KEEFI_D4_RENDER_DIR");if(directory.isEmpty())QSKIP("Optional synthetic D4 market overview view PNG artifacts; set KEEFI_D4_RENDER_DIR to export.");
        QVERIFY(QDir().mkpath(directory));const auto original=KeefiTheme::instance()->mode();
        for(const auto mode:{ThemeMode::Light,ThemeMode::Dark}){
            KeefiTheme::instance()->setMode(mode);OverviewViewTransport transport;MarketOverview overview(&transport);MarketOverviewView view(&overview);view.setStyleSheet(KeefiTheme::localStyleSheet(&view));view.resize(1200,420);view.show();QTRY_COMPARE(transport.requests,1);
            const auto date=QDateTime::currentDateTimeUtc().toTimeZone(QTimeZone("America/New_York")).date().toString(Qt::ISODate);HttpResult response;response.status=200;response.body=("<feed xmlns=\"http://www.w3.org/2005/Atom\" xmlns:d=\"http://schemas.microsoft.com/ado/2007/08/dataservices\" xmlns:m=\"http://schemas.microsoft.com/ado/2007/08/dataservices/metadata\"><entry><content><m:properties><d:NEW_DATE>"+date+"T00:00:00</d:NEW_DATE><d:BC_10YEAR>4.25</d:BC_10YEAR></m:properties></content></entry></feed>").toUtf8();transport.complete(response);
            for(const int width:{1280,720}){view.resize(width,420);QCoreApplication::processEvents();QVERIFY(view.grab().save(QDir(directory).filePath(QString("market-overview-%1-%2.png").arg(mode==ThemeMode::Light?"light":"dark").arg(width))));}view.hide();
        }
        KeefiTheme::instance()->setMode(original);
    }
    void ledgerExposesEvidenceWithoutUrlsOrIdentifiers(){
        MarketOverviewView view(nullptr);view.show();
        QVERIFY(view.findChild<QWidget*>("marketOverviewEvidenceFooter"));
        auto* selector=view.findChild<QComboBox*>("marketOverviewMode");QVERIFY(selector);selector->setFocus();
        QTest::keyClick(selector,Qt::Key_End);QCOMPARE(view.mode(),MarketTimeMode::Night);QTest::keyClick(selector,Qt::Key_Home);QCOMPARE(view.mode(),MarketTimeMode::Automatic);
        QTest::mouseClick(selector,Qt::LeftButton,Qt::NoModifier,QPoint(selector->width()-16,selector->height()/2));QTRY_VERIFY(selector->view()->isVisible());
        auto* popup=QApplication::activePopupWidget();QVERIFY(popup);QCOMPARE(popup,selector->view()->window());
        QTest::keyClick(popup,Qt::Key_Escape);QTRY_VERIFY(!selector->view()->isVisible());
    }
    void treasuryCardPreservesOfficialDateAndExposesSourceWithKeyboard(){
        OverviewViewTransport transport;MarketOverview overview(&transport);MarketOverviewView view(&overview);view.resize(900,600);view.show();QTRY_COMPARE(transport.requests,1);
        const auto date=QDateTime::currentDateTimeUtc().toTimeZone(QTimeZone("America/New_York")).date().toString(Qt::ISODate);
        HttpResult response;response.status=200;response.body=("<feed xmlns=\"http://www.w3.org/2005/Atom\" xmlns:d=\"http://schemas.microsoft.com/ado/2007/08/dataservices\" xmlns:m=\"http://schemas.microsoft.com/ado/2007/08/dataservices/metadata\"><entry><content><m:properties><d:NEW_DATE>"+date+"T00:00:00</d:NEW_DATE><d:BC_10YEAR>4.25</d:BC_10YEAR></m:properties></content></entry></feed>").toUtf8();transport.complete(response);QVERIFY(overview.treasury10Year().available);
        auto* card=view.findChild<ExploreMetricCard*>("marketOverviewTreasuryCard");QVERIFY(card);QCOMPARE(card->data().value,QString("4.25"));QCOMPARE(card->data().unit,QString("%"));QCOMPARE(card->data().direction,MovementDirection::Unknown);
        view.setEvidenceStart(4);QCOMPARE(view.evidenceCount(),1);QVERIFY(card->findChild<QLabel*>("metricEvidenceReference")->isHidden());
        auto* footer=view.findChild<ExploreEvidenceFooter*>("marketOverviewEvidenceFooter");QVERIFY(footer);QVERIFY(footer->isHidden());
        QCOMPARE(footer->entries(),card->data().evidence);QCOMPARE(footer->entries().first().sourceDate,date);
        QCOMPARE(footer->entries().first().provider,QStringLiteral("미국 재무부"));QCOMPARE(footer->entries().first().state,QStringLiteral("일별 공시"));
        QCOMPARE(card->data().evidence.first().sourceTimeMs,qint64(0));
        QVERIFY(card->data().freshnessText.contains(date));QVERIFY(!card->historyVisible());QVERIFY(card->plottedHistory().isEmpty());QVERIFY(card->findChild<QWidget*>("metricSparkline")->isHidden());
        auto* info=card->informationButton();QVERIFY(info);info->setFocus();QTest::keyClick(info,Qt::Key_Return);QTRY_VERIFY(info->informationOpen());auto* dialog=info->findChild<QDialog*>("infoDialog");QVERIFY(dialog);
        auto* text=dialog->findChild<QPlainTextEdit*>("infoPublicText");QVERIFY(text);QVERIFY(text->toPlainText().contains(QStringLiteral("미국 재무부")));QVERIFY(!text->toPlainText().contains("https://"));QVERIFY(!text->toPlainText().contains(QStringLiteral("수신")));QCOMPARE(card->data().evidence.first().sourceDate,date);
        QTest::keyClick(dialog,Qt::Key_Escape);QTRY_VERIFY(!info->informationOpen());QCOMPARE(transport.requests,1);QCoreApplication::processEvents();QVERIFY(card->height()<250);QVERIFY(view.findChild<QLabel*>("marketOverviewTitle")->geometry().top()<40);QVERIFY(keefi::test::savePublicViewPreview(view,"c9-market-overview"));
    }
    void showsDailyValueSourceAndManualKstContext(){
        OverviewViewTransport transport;MarketOverview overview(&transport);MarketOverviewView view(&overview);view.resize(720,360);view.show();
        QTRY_COMPARE(transport.requests,1);
        auto* mode=view.findChild<QComboBox*>("marketOverviewMode");QVERIFY(mode);QVERIFY(mode->minimumHeight()>=44);QVERIFY(!mode->accessibleName().isEmpty());
        view.setMode(MarketTimeMode::Night);QCOMPARE(view.mode(),MarketTimeMode::Night);QVERIFY(view.findChild<ExploreMetricCard*>("marketOverviewTreasuryCard")->data().publicInformation.contains("밤"));
        const auto date=QDateTime::currentDateTimeUtc().toTimeZone(QTimeZone("America/New_York")).date().toString(Qt::ISODate);
        HttpResult response;response.status=200;response.body=("<feed xmlns=\"http://www.w3.org/2005/Atom\" xmlns:d=\"http://schemas.microsoft.com/ado/2007/08/dataservices\" xmlns:m=\"http://schemas.microsoft.com/ado/2007/08/dataservices/metadata\"><entry><content><m:properties><d:NEW_DATE>"+date+"T00:00:00</d:NEW_DATE><d:BC_10YEAR>4.25</d:BC_10YEAR></m:properties></content></entry></feed>").toUtf8();
        transport.complete(response);auto* card=view.findChild<ExploreMetricCard*>("marketOverviewTreasuryCard");QVERIFY(card);QCOMPARE(card->data().value,QString("4.25"));QCOMPARE(card->data().unit,QString("%"));
        QVERIFY(card->data().freshnessText.contains("일별 공시"));QVERIFY(card->data().freshnessText.contains(date));
        auto* info=card->informationButton();QVERIFY(info);QVERIFY(!info->informationOpen());QTest::mouseClick(info,Qt::LeftButton);QTRY_VERIFY(info->informationOpen());
        auto* dialog=info->findChild<QDialog*>("infoDialog");QVERIFY(dialog);const auto evidence=dialog->findChild<QPlainTextEdit*>("infoPublicText")->toPlainText();
        QVERIFY(evidence.contains(QStringLiteral("미국 재무부")));QVERIFY(!evidence.contains("https://"));QCOMPARE(card->data().evidence.first().sourceDate,date);QTest::keyClick(dialog,Qt::Key_Escape);
    }
    void failedSourceShowsReasonAndHideStopsPendingRead(){
        OverviewViewTransport transport;MarketOverview overview(&transport);MarketOverviewView view(&overview);view.show();QTRY_COMPARE(transport.requests,1);
        HttpResult response;response.status=503;transport.complete(response);
        const auto card=view.findChild<ExploreMetricCard*>("marketOverviewTreasuryCard")->data();QVERIFY(card.value.contains("실패"));QVERIFY(card.unit.isEmpty());
        OverviewViewTransport held;MarketOverview heldOverview(&held);MarketOverviewView heldView(&heldOverview);heldView.show();QTRY_COMPARE(held.requests,1);
        heldView.hide();QCOMPARE(held.aborts,1);QVERIFY(!heldOverview.busy());
    }
};
}
QTEST_MAIN(keefi::MarketOverviewViewTests)
#include "tst_market_overview_view.moc"
