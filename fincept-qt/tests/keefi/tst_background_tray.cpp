#include "keefi/BackgroundTray.h"
#include <QTest>
using namespace keefi;
class BackgroundTrayTest:public QObject {
    Q_OBJECT
  private slots:
    void closeDecision_data();
    void closeDecision();
};
void BackgroundTrayTest::closeDecision_data(){
    QTest::addColumn<bool>("quitting");QTest::addColumn<bool>("preference");
    QTest::addColumn<bool>("available");QTest::addColumn<bool>("visible");QTest::addColumn<bool>("hintSeen");
    QTest::addColumn<bool>("hide");QTest::addColumn<bool>("explanation");
    for(int mask=0;mask<32;++mask){
        const bool quitting=mask&1,preference=mask&2,available=mask&4,visible=mask&8,hintSeen=mask&16;
        // The two approved hide rows are explicit policy examples; every
        // unavailable/hidden-tray, opt-out, or explicit-quit row must quit.
        const bool keepRunning=mask==14||mask==30;
        const auto name=QString("quit%1-pref%2-available%3-visible%4-seen%5").arg(quitting).arg(preference).arg(available).arg(visible).arg(hintSeen).toUtf8();
        QTest::newRow(name.constData())<<quitting<<preference<<available<<visible<<hintSeen<<keepRunning<<(mask==14);
    }
}
void BackgroundTrayTest::closeDecision(){
    QFETCH(bool,quitting);QFETCH(bool,preference);QFETCH(bool,available);QFETCH(bool,visible);QFETCH(bool,hintSeen);
    QFETCH(bool,hide);QFETCH(bool,explanation);
    const auto decision=backgroundCloseDecision(quitting,preference,available,visible,hintSeen);
    QCOMPARE(decision.action,hide?BackgroundCloseAction::Hide:BackgroundCloseAction::Quit);
    QCOMPARE(decision.showExplanation,explanation);
}
QTEST_GUILESS_MAIN(BackgroundTrayTest)
#include "tst_background_tray.moc"
