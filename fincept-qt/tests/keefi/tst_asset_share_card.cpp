#include "keefi/AssetShareCard.h"
#include "keefi/AssetShareDialog.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/MarketCandles.h"
#include "keefi/PortfolioDetail.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>
using namespace keefi;
namespace {
constexpr qint64 Start = 1793973600000, Minute = 60000, Interval = 15*Minute;
AssetShareSnapshot syntheticSnapshot(bool falling = false, int count = 97) {
    AssetShareSnapshot value;
    value.assetName="가상 자산";value.assetSymbol="SYN";
    value.requestedFromMs=Start+Interval;value.requestedToMs=Start+count*Interval;
    auto& market = value.market;
    market.identity = {"coin","upbit","KRW-SYN"};
    market.currency = "KRW"; market.intervalMs = Interval; market.displayTimeZoneId = "Asia/Seoul";
    for (int i=0;i<count;++i) {
        const auto close = QString::number(falling ? 200-i : 100+i);
        market.closed.append({Start+i*Interval,Start+(i+1)*Interval-1,close,close,close,close,"10"});
    }
    market.calculatedThroughMs = Start+count*Interval; market.asOfMs = market.calculatedThroughMs+1000;
    market.viewport = {Start,Start+(count-1)*Interval};
    market.studies = keefiChartStudies(market.closed,Interval);
    market.source.provider = ChartShareSource::Upbit;
    market.source.canonicalUrl = MarketCandles::request(market.identity.key(),Interval).url.toString();
    market.source.receivedAtMs = market.calculatedThroughMs+500;
    market.availability = ChartShareAvailability::Ready;
    return value;
}
AssetShareSnapshot periodSnapshot(AssetSharePeriod period) {
    auto value=syntheticSnapshot();value.period=period;auto& market=value.market;
    market.intervalMs=AssetShareCard::intervalMs(period);market.closed.clear();
    const int intervals=int(AssetShareCard::durationMs(period)/market.intervalMs);
    const auto begin=Start-Start%market.intervalMs;
    for(int i=0;i<=intervals;++i){const auto price=QString::number(100+i);
        market.closed.append({begin+i*market.intervalMs,begin+(i+1)*market.intervalMs-1,price,price,price,price,"10"});}
    market.viewport={begin,market.closed.last().startsAtMs};market.calculatedThroughMs=begin+(intervals+1)*market.intervalMs;
    market.asOfMs=market.calculatedThroughMs+1000;market.source.receivedAtMs=market.calculatedThroughMs+500;
    market.source.canonicalUrl=MarketCandles::request(market.identity.key(),market.intervalMs).url.toString();
    market.studies=keefiChartStudies(market.closed,market.intervalMs);
    value.requestedFromMs=begin+market.intervalMs;value.requestedToMs=market.calculatedThroughMs;return value;
}
AssetShareSnapshot withPersonalValues() {
    auto value=syntheticSnapshot();value.personal=AssetSharePersonalValues{"78123.456789","987654.321","KRW",Start+97*Interval};return value;
}
QImage expectedNumberLine(const QSize& size,const QString& value,FontRole role,const QColor& color,qreal scale) {
    QImage image(size,QImage::Format_ARGB32_Premultiplied);
    image.setDotsPerMeterX(3780);image.setDotsPerMeterY(3780);image.fill(KeefiTheme::colorsFor(true).bg);
    QPainter painter(&image);painter.setRenderHint(QPainter::Antialiasing);painter.scale(scale,scale);
    painter.setFont(KeefiFonts::font(role));painter.setPen(color);
    painter.drawText(QRectF(0,0,size.width()/scale,size.height()/scale),
        Qt::AlignLeft|Qt::AlignVCenter|Qt::TextSingleLine,value);
    return image;
}
}
class AssetShareCardTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { QVERIFY(KeefiFonts::registerBundledFonts()); }
    void displayedPeriodUsesActualPublicPrices_data() {
        QTest::addColumn<bool>("falling"); QTest::addColumn<QString>("first"); QTest::addColumn<QString>("last");
        QTest::addColumn<QString>("amount"); QTest::addColumn<QString>("percent");
        QTest::newRow("rising") << false << QString("100") << QString("196") << QString("+96") << QString("+96.00%");
        QTest::newRow("falling") << true << QString("200") << QString("104") << QString("-96") << QString("-48.00%");
    }
    void displayedPeriodUsesActualPublicPrices() {
        QFETCH(bool,falling); QFETCH(QString,first); QFETCH(QString,last); QFETCH(QString,amount); QFETCH(QString,percent);
        const auto result = AssetShareCard::render(syntheticSnapshot(falling));
        QVERIFY2(result.available,qPrintable(result.reason));
        QCOMPARE(result.startPrice,first); QCOMPARE(result.currentPrice,last);
        QCOMPARE(result.changeAmount,amount); QCOMPARE(result.changePercent,percent);
        QCOMPARE(result.fromMs,Start+Interval); QCOMPARE(result.toMs,Start+97*Interval);
        QCOMPARE(result.image.size(),QSize(1000,1200)); QVERIFY(result.image.textKeys().isEmpty());
        const auto text = result.captions.join('\n');
        QVERIFY(text.contains("시장 가격 변화")); QVERIFY(text.contains("시작")); QVERIFY(text.contains("현재"));
        QVERIFY(!QString(text).remove("원가·수익률 아님").contains("수익")); QVERIFY(!text.contains("P/L")); QVERIFY(!text.contains("Entry"));
    }
    void largestMarketChangeNumberIsThePeriodPercent_data() {
        QTest::addColumn<int>("direction");QTest::addColumn<QString>("percent");QTest::addColumn<QString>("amountLine");
        QTest::newRow("rising")<<1<<QString("+96.00%")<<QString("+96 KRW");
        QTest::newRow("falling")<<-1<<QString("-48.00%")<<QString("-96 KRW");
        QTest::newRow("flat")<<0<<QString("0.00%")<<QString("0 KRW");
    }
    void largestMarketChangeNumberIsThePeriodPercent() {
        QFETCH(int,direction);QFETCH(QString,percent);QFETCH(QString,amountLine);
        auto value=syntheticSnapshot(direction<0);
        if(direction==0){
            for(auto& row:value.market.closed)row.open=row.high=row.low=row.close="100";
            value.market.studies=keefiChartStudies(value.market.closed,value.market.intervalMs);
        }
        const auto result=AssetShareCard::render(value);QVERIFY2(result.available,qPrintable(result.reason));
        const auto& colors=KeefiTheme::colorsFor(true);
        const auto movement=direction>0?colors.upText:direction<0?colors.downText:colors.text;
        // Compare actual glyph pixels before checking metadata: a caption-only
        // change must not pass while the largest visible number remains KRW.
        QVERIFY2(result.image.copy(QRect(68,588,864,112))==
            expectedNumberLine({864,112},percent,FontRole::Hero44,movement,2),
            "The largest visible market-change number must be the period percent in Hero44");
        QVERIFY2(result.image.copy(QRect(68,708,864,64))==
            expectedNumberLine({864,64},amountLine,FontRole::Lead28,movement,1),
            "The absolute price change must remain below the percent in smaller Lead28 text");
        QCOMPARE(result.heroText,percent);QCOMPARE(result.heroText,result.changePercent);
        QVERIFY(result.captions.contains("시장 가격 변화"));QVERIFY(result.captions.contains(amountLine));
    }
    void assetTitleAndKstAreSeparateFromTheMarket() {
        const auto result=AssetShareCard::render(syntheticSnapshot());QVERIFY(result.available);
        QCOMPARE(result.captions.first(),QStringLiteral("가상 자산 SYN"));
        const auto text=result.captions.join('\n');QVERIFY(text.contains("업비트 KRW-SYN"));
        QVERIFY(text.contains("KST (UTC+9)"));QVERIFY(!text.contains("Asia/Seoul"));
    }
    void exactPeriodEndpointsAreRequired_data() {
        QTest::addColumn<int>("period");
        for(int i=0;i<4;++i)QTest::newRow(qPrintable(QString::number(i)))<<i;
    }
    void exactPeriodEndpointsAreRequired() {
        QFETCH(int,period);const auto value=periodSnapshot(AssetSharePeriod(period));
        QVERIFY(value.market.closed.size()<=ChartShareCard::MaxClosedBars);
        const auto complete=AssetShareCard::render(value);QVERIFY2(complete.available,qPrintable(complete.reason));
        QCOMPARE(complete.toMs-complete.fromMs,AssetShareCard::durationMs(value.period));
        auto missing=value;missing.market.closed.removeFirst();missing.market.viewport.fromMs=missing.market.closed.first().startsAtMs;
        missing.market.studies=keefiChartStudies(missing.market.closed,missing.market.intervalMs);
        const auto noStart=AssetShareCard::render(missing);QVERIFY(!noStart.available);QVERIFY(noStart.reason.contains("시작 봉"));
        missing=value;missing.market.closed.removeLast();missing.market.viewport.toMs=missing.market.closed.last().startsAtMs;
        missing.market.calculatedThroughMs=missing.market.closed.last().startsAtMs+missing.market.intervalMs;
        missing.market.studies=keefiChartStudies(missing.market.closed,missing.market.intervalMs);
        const auto noEnd=AssetShareCard::render(missing);QVERIFY(!noEnd.available);QVERIFY(noEnd.reason.contains("끝 봉"));
    }
    void personalFieldsAreIndependent_data() {
        QTest::addColumn<QString>("field");for(const auto* field:{"quantity","valuation","custody","today"})QTest::newRow(field)<<QString(field);
    }
    void personalFieldsAreIndependent() {
        QFETCH(QString,field);auto value=withPersonalValues();value.personal->custodyProviders={"phantom","upbit"};
        const auto at=QDateTime::fromMSecsSinceEpoch(value.personal->observedAtMs,QTimeZone("Asia/Seoul"));
        value.personal->today=AssetShareTodayObservation{"+123.45","KRW",at.date().startOfDay(at.timeZone()).toMSecsSinceEpoch(),value.personal->observedAtMs};
        AssetShareOptions options;options.includeQuantity=field=="quantity";options.includeValuation=field=="valuation";
        options.includeCustody=field=="custody";options.includeTodayObservation=field=="today";
        const auto result=AssetShareCard::render(value,options);QVERIFY2(result.available,qPrintable(result.reason));
        const auto text=result.captions.join('\n');QVERIFY(result.personalValuesIncluded);
        QCOMPARE(text.contains("78,123.456789"),options.includeQuantity);
        QCOMPARE(text.contains("987,654.321"),options.includeValuation);
        QCOMPARE(text.contains("Phantom 지갑"),options.includeCustody);
        QCOMPARE(text.contains("+123.45"),options.includeTodayObservation);
        if(options.includeValuation)QVERIFY(text.contains("KEEFI 평가 · 부분"));
        if(options.includeTodayObservation)QVERIFY(text.contains("관측 평가액 증감 · 입출금 포함 가능 · 오늘 0시 이후"));
        QCOMPARE(result.changeAmount,AssetShareCard::render(value).changeAmount);
    }
    void todayObservationUsesTheFirstJointRecordAfterMidnight_data() {
        QTest::addColumn<qint64>("offset");QTest::addColumn<bool>("available");
        QTest::newRow("midnight")<<qint64(0)<<true;
        QTest::newRow("first-joint-0005")<<qint64(5*Minute)<<true;
        QTest::newRow("previous-day")<<qint64(-Minute)<<false;
    }
    void todayObservationUsesTheFirstJointRecordAfterMidnight() {
        QFETCH(qint64,offset);QFETCH(bool,available);auto value=withPersonalValues();
        const auto end=QDateTime::fromMSecsSinceEpoch(value.personal->observedAtMs,QTimeZone("Asia/Seoul"));
        const auto start=end.date().startOfDay(end.timeZone()).addMSecs(offset);
        value.personal->today=AssetShareTodayObservation{"+123.45","KRW",start.toMSecsSinceEpoch(),end.toMSecsSinceEpoch()};
        AssetShareOptions options;options.includeTodayObservation=true;const auto result=AssetShareCard::render(value,options);
        QCOMPARE(result.available,available);if(!available)return;
        const auto text=result.captions.join('\n');QVERIFY(text.contains(start.toString("yyyy.MM.dd HH:mm")));
        QVERIFY(text.contains(end.toString("yyyy.MM.dd HH:mm")));QVERIFY(text.contains("오늘 0시 이후"));
    }
    void dialogSelectionRequestsOnlySuppliedPublicMarketsAndPeriods() {
        AssetShareDialog dialog(withPersonalValues());const WatchlistIdentity first{"coin","upbit","KRW-SYN"},second{"coin","binance","SYNUSDT"};
        dialog.setMarkets({first,second},first);dialog.show();
        auto* market=dialog.findChild<QComboBox*>("assetShareMarket");auto* period=dialog.findChild<QComboBox*>("assetSharePeriod");
        QVERIFY(market&&period);QCOMPARE(market->count(),2);QCOMPARE(period->count(),4);
        QSignalSpy markets(&dialog,&AssetShareDialog::marketRequested),periods(&dialog,&AssetShareDialog::periodRequested);
        market->setFocus();QTest::keyClick(market,Qt::Key_Down);
        QCOMPARE(markets.size(),1);QCOMPARE(qvariant_cast<WatchlistIdentity>(markets.first().first()),second);QVERIFY(!dialog.previewAvailable());
        dialog.setSnapshot(withPersonalValues());period->setFocus();QTest::keyClick(period,Qt::Key_Down);
        QCOMPARE(periods.size(),1);QCOMPARE(qvariant_cast<AssetSharePeriod>(periods.first().first()),AssetSharePeriod::Week);
        QVERIFY(!dialog.previewAvailable());
    }
    void dialogKeepsVerifiedStockCandidates() {
        AssetShareDialog dialog(syntheticSnapshot());const WatchlistIdentity stock{"stock","KR","005930"};
        dialog.setMarkets({stock},stock);const auto* market=dialog.findChild<QComboBox*>("assetShareMarket");QVERIFY(market);
        QCOMPARE(market->count(),1);QCOMPARE(market->currentText(),QStringLiteral("한국 주식 005930"));
    }
    void replacingSelectedMarketRevokesTheOldPreview() {
        AssetShareDialog dialog(syntheticSnapshot());dialog.show();QVERIFY(dialog.previewAvailable());
        const WatchlistIdentity second{"coin","binance","SYNUSDT"};dialog.setMarkets({second},second);
        QVERIFY(!dialog.previewAvailable());const auto* save=dialog.findChild<QPushButton*>("assetShareSave");QVERIFY(save);QVERIFY(!save->isEnabled());
    }
    void personalControlsStartOffAndExplainUnavailableToday() {
        auto value=withPersonalValues();value.personal->todayUnavailableReason="오늘 비교의 시작 기록이 없어요.";
        AssetShareDialog dialog(value);dialog.show();
        for(const auto* name:{"assetShareQuantity","assetShareValuation","assetShareCustody","assetShareTodayObservation"}){
            const auto* box=dialog.findChild<QCheckBox*>(name);QVERIFY(box);QVERIFY(!box->isChecked());}
        const auto* today=dialog.findChild<QCheckBox*>("assetShareTodayObservation");QVERIFY(!today->isEnabled());
        const auto* reason=dialog.findChild<QLabel*>("assetShareTodayReason");QVERIFY(reason);
        QCOMPARE(reason->text(),value.personal->todayUnavailableReason);QCOMPARE(reason->textFormat(),Qt::PlainText);
    }
    void dialogFitsTheWindowWithoutDistortingTheCard_data() {
        QTest::addColumn<QSize>("size");QTest::addColumn<bool>("largeText");
        QTest::newRow("compact")<<QSize(1100,760)<<false;
        QTest::newRow("compact-large-text")<<QSize(1100,760)<<true;
        QTest::newRow("app-default")<<QSize(1360,900)<<false;
        QTest::newRow("app-default-large-text")<<QSize(1360,900)<<true;
    }
    void dialogFitsTheWindowWithoutDistortingTheCard() {
        QFETCH(QSize,size);QFETCH(bool,largeText);AssetShareDialog dialog(withPersonalValues());
        dialog.setMarkets({{"coin","upbit","KRW-SYN"}},{"coin","upbit","KRW-SYN"});
        if(largeText)for(auto* widget:dialog.findChildren<QWidget*>())if(widget->objectName()!="assetSharePreview"){
            auto font=widget->font();font.setPixelSize(20);widget->setFont(font);}
        dialog.resize(size);dialog.show();QCoreApplication::processEvents();QCOMPARE(dialog.size(),size);
        for(const auto* name:{"assetShareMarket","assetSharePeriod","assetShareQuantity","assetShareValuation",
             "assetShareCustody","assetShareTodayObservation","assetShareSave","assetShareClose"}){
            const auto* control=dialog.findChild<QWidget*>(name);QVERIFY(control);QVERIFY(control->isVisible());
            QVERIFY2(dialog.rect().contains(QRect(control->mapTo(&dialog,QPoint()),control->size())),name);
        }
        const auto* preview=dialog.findChild<QLabel*>("assetSharePreview");QVERIFY(preview);const auto pixmap=preview->pixmap();QVERIFY(!pixmap.isNull());
        QVERIFY(qAbs(qreal(pixmap.width())/pixmap.height()-1000.0/1200.0)<0.005);
        QCOMPARE(dialog.previewImage(),AssetShareCard::render(withPersonalValues()).image);
        const auto output=qEnvironmentVariable("KEEFI_ASSET_SHARE_CAPTURE_DIR");
        if(!output.isEmpty())QVERIFY(dialog.grab().save(QDir(output).filePath(QString("synthetic-dialog-%1-%2.png").arg(size.width()).arg(largeText?"large-text":"normal")),"PNG"));
    }
    void personalNumbersAreOptInAndDoNotChangeMarketFigures() {
        const auto publicOnly=AssetShareCard::render(syntheticSnapshot());const auto value=withPersonalValues();
        const auto hidden=AssetShareCard::render(value);QVERIFY(hidden.available);
        QCOMPARE(hidden.image,publicOnly.image);QCOMPARE(hidden.captions,publicOnly.captions);QVERIFY(!hidden.personalValuesIncluded);
        AssetShareOptions options;options.includeQuantity=true;options.includeValuation=true;
        const auto shown=AssetShareCard::render(value,options);QVERIFY2(shown.available,qPrintable(shown.reason));
        QVERIFY(shown.personalValuesIncluded);QVERIFY(shown.image!=hidden.image);
        QCOMPARE(shown.changeAmount,hidden.changeAmount);QCOMPARE(shown.changePercent,hidden.changePercent);
        const auto captions=shown.captions.join('\n');QVERIFY(captions.contains("현재 수량"));QVERIFY(captions.contains("현재 평가액"));
        QVERIFY(captions.contains("78,123.456789"));QVERIFY(captions.contains("987,654.321"));
        QVERIFY(!QString(captions).remove("원가·수익률 아님").contains("수익"));QVERIFY(!captions.contains("P/L"));QVERIFY(!captions.contains("Entry"));
        auto invalid=value;invalid.personal->quantity="PRIVATE_ALIAS_CANARY";
        QVERIFY(!AssetShareCard::render(invalid,options).available);QCOMPARE(AssetShareCard::render(invalid).image,publicOnly.image);
    }
    void invalidSourcesAndFuturePricesCannotRender() {
        auto value=syntheticSnapshot();value.market.source.canonicalUrl="https://api.upbit.com/v1/accounts?PRIVATE_CANARY";
        const auto badSource=AssetShareCard::render(value);QVERIFY(!badSource.available);QVERIFY(badSource.image.isNull());QVERIFY(!badSource.reason.contains("PRIVATE_CANARY"));
        value=syntheticSnapshot();value.market.source.receivedAtMs=value.market.asOfMs+1;QVERIFY(!AssetShareCard::render(value).available);
        value=syntheticSnapshot();value.market.availability=ChartShareAvailability::Unavailable;QVERIFY(!AssetShareCard::render(value).available);
    }
    void flatAndDelayedPricesHaveHonestLabels() {
        auto value=syntheticSnapshot();for(auto& row:value.market.closed)row.open=row.high=row.low=row.close="100";
        value.market.studies=keefiChartStudies(value.market.closed,value.market.intervalMs);
        const auto flat=AssetShareCard::render(value);QVERIFY(flat.available);QCOMPARE(flat.changeAmount,QString("0"));QCOMPARE(flat.changePercent,QString("0.00%"));
        value.market.availability=ChartShareAvailability::Stale;
        const auto stale=AssetShareCard::render(value);QVERIFY(stale.available);QVERIFY(stale.captions.join('\n').contains("지연"));
    }
    void sparklineDoesNotBridgeMissingPrices() {
        auto value=syntheticSnapshot();const auto complete=AssetShareCard::render(value);QVERIFY(complete.available);
        value.market.closed.removeAt(40);value.market.studies=keefiChartStudies(value.market.closed,Interval);
        const auto missing=AssetShareCard::render(value);QVERIFY2(missing.available,qPrintable(missing.reason));
        const int x=qRound(68+40.0/96*864),y=qRound(500-40.0/96*290);
        const auto count=[&](const QImage& image){int pixels=0;for(int xx=x-3;xx<=x+3;++xx)for(int yy=y-3;yy<=y+3;++yy)
            if(image.pixelColor(xx,yy)==KeefiTheme::colorsFor(true).upText)++pixels;return pixels;};
        QVERIFY(count(complete.image)>0);QCOMPARE(count(missing.image),0);
        QVERIFY(missing.captions.join('\n').contains("자료 없는 구간"));
    }
    void sparklinePreservesSmallExactPriceChanges() {
        auto value=syntheticSnapshot();
        for(int i=0;i<value.market.closed.size();++i){
            const auto price=QStringLiteral("100000000.000000000")+QString::number(i+1).rightJustified(2,'0');
            auto& row=value.market.closed[i];row.open=row.high=row.low=row.close=price;
        }
        value.market.studies=keefiChartStudies(value.market.closed,Interval);
        const auto result=AssetShareCard::render(value);QVERIFY2(result.available,qPrintable(result.reason));
        QCOMPARE(result.changeAmount,QStringLiteral("+0.00000000096"));
        // Strictly increasing source prices must end at the chart's high edge,
        // even when absolute double conversion collapses every quote to 1e8.
        QCOMPARE(result.image.pixelColor(932,210),KeefiTheme::colorsFor(true).upText);
    }
    void pngExportIsExplicitAndHasNoMetadata() {
        QTemporaryDir directory;QVERIFY(directory.isValid());const auto path=directory.filePath("synthetic-public.png");QString error;
        const auto value=withPersonalValues();QVERIFY2(AssetShareCard::savePng(value,{},path,&error),qPrintable(error));
        QImageReader reader(path,"png");QVERIFY(reader.canRead());QVERIFY(reader.textKeys().isEmpty());
        QCOMPARE(reader.read().convertToFormat(QImage::Format_ARGB32),AssetShareCard::render(value).image.convertToFormat(QImage::Format_ARGB32));
        QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));const auto before=file.readAll();file.close();
        QVERIFY(before.startsWith(QByteArray::fromHex("89504e470d0a1a0a")));
        QStringList chunks;qsizetype at=8;
        while(at<before.size()){
            QVERIFY(at+12<=before.size());quint32 size=0;
            for(int i=0;i<4;++i)size=(size<<8)|quint8(before[at+i]);
            QVERIFY(quint64(at)+12+size<=quint64(before.size()));
            const auto kind=QString::fromLatin1(before.mid(at+4,4));chunks.append(kind);
            QVERIFY2((QStringList{"IHDR","IDAT","IEND","pHYs"}.contains(kind)),qPrintable(kind));
            at+=12+size;
        }
        QCOMPARE(at,before.size());QCOMPARE(chunks.first(),QString("IHDR"));QCOMPARE(chunks.last(),QString("IEND"));
        QVERIFY(!before.contains(directory.path().toUtf8()));QVERIFY(!before.contains("PRIVATE_ALIAS_CANARY"));
        auto invalid=value;invalid.market.availability=ChartShareAvailability::Unavailable;
        QVERIFY(!AssetShareCard::savePng(invalid,{},path,&error));QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),before);
        QVERIFY(!AssetShareCard::savePng(value,{},"relative.png",&error));
    }
    void dialogRevokesPendingAndLatePersonalExports() {
        QTemporaryDir directory;const auto path=directory.filePath("must-not-save.png");
        AssetShareDialog dialog(withPersonalValues());dialog.show();QVERIFY(dialog.previewAvailable());
        auto* personal=dialog.findChild<QCheckBox*>("assetShareQuantity");auto* save=dialog.findChild<QPushButton*>("assetShareSave");QVERIFY(personal&&save);
        QVERIFY(!personal->isChecked());personal->setChecked(true);QVERIFY(dialog.options().includeQuantity);
        QSignalSpy saved(&dialog,&AssetShareDialog::saved);int choices=0;
        dialog.setPathChooser([&](QWidget*){++choices;dialog.clearPersonalValues();return path;});save->click();
        QCOMPARE(choices,1);QCOMPARE(saved.size(),0);QVERIFY(!QFile::exists(path));
        QVERIFY(!dialog.snapshot().personal);QVERIFY(!personal->isChecked());QVERIFY(!personal->isEnabled());
        dialog.setSnapshot(withPersonalValues());QVERIFY(!dialog.snapshot().personal);QVERIFY(!personal->isEnabled());
        QCOMPARE(dialog.previewImage(),AssetShareCard::render(syntheticSnapshot()).image);
    }
    void dialogSnapshotsAreFrozenAndCancelNeverWrites() {
        QTemporaryDir directory;const auto path=directory.filePath("saved.png");auto input=syntheticSnapshot();
        AssetShareDialog dialog(input);input.market.closed.clear();dialog.show();QVERIFY(dialog.previewAvailable());
        auto* save=dialog.findChild<QPushButton*>("assetShareSave");QVERIFY(save);int choices=0;QSignalSpy saved(&dialog,&AssetShareDialog::saved);
        dialog.setPathChooser([&](QWidget*){++choices;return QString{};});dialog.resize(720,900);QCOMPARE(choices,0);save->click();
        QCOMPARE(choices,1);QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
        dialog.setPathChooser([&](QWidget*){dialog.setSnapshot(syntheticSnapshot(true));return path;});save->click();
        QCOMPARE(saved.size(),0);QVERIFY(!QFile::exists(path));
        dialog.setPathChooser([&](QWidget*){return path;});save->click();QCOMPARE(saved.size(),1);
        QImageReader reader(path,"png");QCOMPARE(reader.read().convertToFormat(QImage::Format_ARGB32),dialog.previewImage().convertToFormat(QImage::Format_ARGB32));
    }
    void myAssetsEntryDropsSelectionOnClearAndPublicTransition() {
        AssetDetailPanel panel;panel.enableAssetShareControl();panel.resize(400,900);panel.show();
        PortfolioGroupView group;group.identity="solana:native";group.asset="SOL";
        panel.showGroup(group);auto* button=panel.findChild<QPushButton*>("assetDetailShare");QVERIFY(button);QVERIFY(button->isVisible());
        QSignalSpy requested(&panel,&AssetDetailPanel::assetShareRequested);button->click();QCOMPARE(requested.size(),1);
        QCOMPARE(requested.first().first().toString(),QString("solana:native"));
        panel.clearDetail();QVERIFY(button->isHidden());button->click();QCOMPARE(requested.size(),1);
        panel.showGroup(group);QVERIFY(button->isVisible());panel.showQuote("SYN","SYN","100","1",{},{});
        QVERIFY(button->isHidden());button->click();QCOMPARE(requested.size(),1);
    }
    void captureSyntheticCardsWhenAsked() {
        const auto output=qEnvironmentVariable("KEEFI_ASSET_SHARE_CAPTURE_DIR");
        if(output.isEmpty())QSKIP("Synthetic card capture is opt-in only");
        QVERIFY(QDir::isAbsolutePath(output));QVERIFY(QDir().mkpath(output));QString error;
        QVERIFY2(AssetShareCard::savePng(syntheticSnapshot(),{},QDir(output).filePath("synthetic-asset-rising.png"),&error),qPrintable(error));
        QVERIFY2(AssetShareCard::savePng(syntheticSnapshot(true),{},QDir(output).filePath("synthetic-asset-falling.png"),&error),qPrintable(error));
        auto flat=syntheticSnapshot();for(auto& row:flat.market.closed)row.open=row.high=row.low=row.close="100";
        flat.market.studies=keefiChartStudies(flat.market.closed,flat.market.intervalMs);
        QVERIFY2(AssetShareCard::savePng(flat,{},QDir(output).filePath("synthetic-asset-flat.png"),&error),qPrintable(error));
        auto personal=withPersonalValues();personal.personal->custodyProviders={"phantom","upbit"};
        const auto at=QDateTime::fromMSecsSinceEpoch(personal.personal->observedAtMs,QTimeZone("Asia/Seoul"));
        personal.personal->today=AssetShareTodayObservation{"+123.45","KRW",at.date().startOfDay(at.timeZone()).toMSecsSinceEpoch(),personal.personal->observedAtMs};
        AssetShareOptions options;options.includeQuantity=options.includeValuation=options.includeCustody=options.includeTodayObservation=true;
        QVERIFY2(AssetShareCard::savePng(personal,options,QDir(output).filePath("synthetic-asset-personal-example.png"),&error),qPrintable(error));
    }
};
QTEST_MAIN(AssetShareCardTest)
#include "tst_asset_share_card.moc"
