#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/ChartDrawings.h"
#include "keefi/ChartShareCard.h"
#include "keefi/ChartStudyPreferences.h"
#include "keefi/MarketCandles.h"
#include "keefi/MarketCandlesView.h"
#include "keefi/StockChartSource.h"
#include "keefi/TradingTransport.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QTimeZone>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <cmath>
#include <cstring>
using namespace keefi;
namespace {
constexpr qint64 Minute=60000,Day=86400000,Start=1792713600000;
ChartShareSnapshot snapshot(int count=81) {
    ChartShareSnapshot value;value.identity={"coin","upbit","KRW-SYN"};value.currency="KRW";value.intervalMs=Minute;value.displayTimeZoneId="Asia/Seoul";
    for(int i=0;i<count;++i)value.closed.append({Start+i*Minute,Start+(i+1)*Minute-1,"100","101","99","100","0"});
    if(count>11){value.closed[10].low="90";value.closed[11].high="120";}
    if(count>80){value.closed[80].open="110";value.closed[80].high="111";value.closed[80].low="109";value.closed[80].close="110";}
    value.calculatedThroughMs=Start+count*Minute;value.asOfMs=value.calculatedThroughMs+1000;
    value.viewport={Start+qMax(0,count-60)*Minute,Start+(count-1)*Minute};
    value.source.provider=ChartShareSource::Upbit;value.source.canonicalUrl=MarketCandles::request(value.identity.key(),Minute).url.toString();
    value.source.receivedAtMs=value.calculatedThroughMs+500;value.studies=keefiChartStudies(value.closed,Minute);
    value.visiblePublicLayers={"pullback","similarity","cloud","volume"};value.availability=ChartShareAvailability::Ready;return value;
}
ChartShareSnapshot candleStyleSnapshot() {
    auto value=snapshot(24);
    // The public observations have symmetric 95..115 wicks, with every body
    // spanning 100..110. The default card's chart centre (y=331) therefore
    // crosses body interiors rather than wick ends in both directions.
    for(int i=0;i<value.closed.size();++i){auto& row=value.closed[i];row.open=i%2?"110":"100";row.close=i%2?"100":"110";
        row.high="115";row.low="95";row.volume="100";}
    value.studies=keefiChartStudies(value.closed,value.intervalMs);value.visiblePublicLayers.clear();return value;
}
int longestInkRunAt(const QImage& image,int y,const QColor& wanted,const QColor& other,const QColor& background) {
    const auto distance=[](const QColor& a,const QColor& b){const int r=a.red()-b.red(),g=a.green()-b.green(),bl=a.blue()-b.blue();return r*r+g*g+bl*bl;};
    int longest=0,current=0;
    for(int x=0;x<image.width();++x){const auto pixel=image.pixelColor(x,y);
        const bool ink=distance(pixel,wanted)<distance(pixel,other)&&distance(pixel,wanted)<distance(pixel,background);
        if(ink)longest=qMax(longest,++current);else current=0;}
    return longest;
}
bool saveF05Render(const QImage& image,const QString& name){
    const auto path=qEnvironmentVariable("KEEFI_F05_RENDER_DIR");if(path.isEmpty())return true;
    return QDir().mkpath(path)&&image.save(QDir(path).filePath(name+".png"));
}
QImage literalGlyphPatch(const QString& literal,FontRole role,const QColor& ink,const QColor& background){
    const auto font=KeefiFonts::font(role);const QFontMetrics metrics(font);
    QImage patch(metrics.horizontalAdvance(literal)+8,metrics.height()+8,QImage::Format_ARGB32_Premultiplied);patch.fill(background);
    {QPainter painter(&patch);painter.setFont(font);painter.setPen(ink);painter.drawText(QPoint(4,4+metrics.ascent()),literal);}
    QRect inkBounds;
    for(int y=0;y<patch.height();++y)for(int x=0;x<patch.width();++x)if(patch.pixelColor(x,y)!=background)
        inkBounds=inkBounds.isNull()?QRect(x,y,1,1):inkBounds.united(QRect(x,y,1,1));
    return inkBounds.isNull()?QImage{}:patch.copy(inkBounds);
}
bool containsLiteralGlyph(const QImage& image,const QRect& search,const QImage& patch){
    if(patch.isNull()||image.format()!=patch.format())return false;const auto area=search.intersected(image.rect());
    for(int y=area.top();y+patch.height()<=area.bottom()+1;++y)for(int x=area.left();x+patch.width()<=area.right()+1;++x){
        bool equal=true;for(int row=0;row<patch.height();++row)if(std::memcmp(image.constScanLine(y+row)+x*4,patch.constScanLine(row),size_t(patch.width())*4)!=0){equal=false;break;}
        if(equal)return true;
    }
    return false;
}
QByteArray pixels(const QImage& image) {return QCryptographicHash::hash(QByteArray(reinterpret_cast<const char*>(image.constBits()),image.sizeInBytes()),QCryptographicHash::Sha256);}
QByteArray read(const QString& path) {QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};}
QString captions(const ChartShareRenderResult& result) {return result.publicCaptions.join('\n');}
QStringList chunks(const QByteArray& raw) {
    QStringList result;if(!raw.startsWith(QByteArray::fromHex("89504e470d0a1a0a")))return result;
    qsizetype at=8;while(at+12<=raw.size()){const quint32 count=qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(raw.constData()+at));
        if(count>quint32(raw.size()-at-12))return {};result.append(QString::fromLatin1(raw.mid(at+4,4)));at+=qsizetype(count)+12;}
    return at==raw.size()?result:QStringList{};
}
ChartShareSnapshot stockSnapshot() {
    auto value=snapshot(30);value.identity={"stock","US","AAPL"};value.currency="USD";value.intervalMs=Day;value.displayTimeZoneId="America/New_York";
    value.closed.clear();ChartStudyTimeline timeline;
    for(int i=0;i<30;++i){const qint64 at=Start+i*Day;value.closed.append({at,0,"100","101","99","100","1000"});ChartStudyTimelineRow row;
        row.startsAtMs=at;row.completesAtMs=at+Day;row.connectsPrevious=i>0;row.eligible=true;row.session=StockSessionKind::Regular;
        timeline.rows.append(row);}
    timeline.adjustedComparable=true;timeline.projectionCovered=false;value.stockTimeline=timeline;
    value.calculatedThroughMs=value.closed.last().startsAtMs+Day;value.asOfMs=Start+31*Day;
    value.viewport={value.closed.first().startsAtMs,value.closed.last().startsAtMs};value.source.sourceTimeMs=0;value.source.receivedAtMs=value.asOfMs-500;
    value.source.provider=ChartShareSource::Toss;value.source.delayed=false;value.source.adjustment=StockAdjustment::SourceAdjusted;
    value.source.canonicalUrl=StockChartSource::request(StockChartRead::Candles,value.identity,Day).url.toString();
    value.studies=keefiChartStudies(value.closed,Day,timeline);return value;
}
}
class ChartShareCardTest final:public QObject {
    Q_OBJECT
  private slots:
    void exportSyntheticPreview();
    void exportedCaptionsAndPixelsOmitInternalEvidence_data();
    void exportedCaptionsAndPixelsOmitInternalEvidence();
    void largePriceKeepsItsExactDecimalDigits();
    void publicCandlesAndScorecardRenderIntoANewDeterministicImage();
    void themeChangesPublicInkWithoutChangingSourceOrCaptions();
    void risingCandlesStayHollowAndFallingCandlesStayFilled_data();
    void risingCandlesStayHollowAndFallingCandlesStayFilled();
    void pullbackZoneUsesDiagonalSixPixelHatching_data();
    void pullbackZoneUsesDiagonalSixPixelHatching();
    void mixedCaptionsKeepNumberAndPriceRoles_data();
    void mixedCaptionsKeepNumberAndPriceRoles();
    void arbitraryReasonsAndLabelsNeverEnterTheImageOrCaptions();
    void invalidPublicInputRejectsBeforeRendering_data();
    void invalidPublicInputRejectsBeforeRendering();
    void suppliedFutureStudyCannotBeAttachedToAnEarlierPrefix_data();
    void suppliedFutureStudyCannotBeAttachedToAnEarlierPrefix();
    void sourceClockZeroStaleAndUnavailableRemainExplicit();
    void noHistoryNeverInventsAPriceOrScorecard();
    void unsupportedLegacyDailySourceCannotBeShared();
    void stockWithoutCompletionProofCannotBeShared();
    void tossUsesTheActualCanonicalSourceRequest();
    void publicDrawingsAreExplicitOptInOnly();
    void fibAndZoneDrawingsRenderAndHiddenDrawingsOmitFromShareSnapshot();
    void movementSchemeChangesPublicInkButKeepsDirectionAndFacts();
    void localPngHasNoPrivateOrArbitraryMetadata();
    void invalidSaveDoesNotReplaceAnExistingFile();
    void saveAcceptsOnlyExplicitAbsolutePngPaths_data();
    void saveAcceptsOnlyExplicitAbsolutePngPaths();
};
void ChartShareCardTest::exportSyntheticPreview() {
    const auto path=qEnvironmentVariable("KEEFI_G8_CHART_PREVIEW");if(path.isEmpty())QSKIP("Optional synthetic public PNG artifact");
    const QDir directory(path);QVERIFY(directory.isAbsolute());
    QVERIFY(QDir().mkpath(directory.absolutePath()));const auto output=directory.filePath("synthetic-public-card.png");
    QString error;QVERIFY2(ChartShareCard::savePng(snapshot(),{},output,&error),qPrintable(error));
    QVERIFY2(ChartShareCard::savePng(stockSnapshot(),{},directory.filePath("synthetic-stock-card.png"),&error),qPrintable(error));
}
void ChartShareCardTest::exportedCaptionsAndPixelsOmitInternalEvidence_data() {
    QTest::addColumn<QString>("market");
    QTest::newRow("upbit")<<QString("upbit");QTest::newRow("binance")<<QString("binance");QTest::newRow("stock")<<QString("stock");
}
void ChartShareCardTest::exportedCaptionsAndPixelsOmitInternalEvidence() {
    QFETCH(QString,market);auto value=market=="stock"?stockSnapshot():snapshot();
    QString code=market=="stock"?QString("AAPL"):QString("SYN/KRW");
    if(market=="binance") {
        value.identity={"coin","binance","SYNUSDT"};value.currency="USDT";value.source.provider=ChartShareSource::BinanceSpot;
        value.source.canonicalUrl=MarketCandles::request(value.identity.key(),value.intervalMs).url.toString();code="SYN/USDT";
    }
    value.source.sourceTimeMs=Start;
    const auto identity=value.identity.key(),url=value.source.canonicalUrl;
    const auto received=value.source.receivedAtMs,completed=value.calculatedThroughMs;
    const auto samples=value.studies.scorecard.samples,overlap=value.studies.scorecard.overlappingSignals;
    const auto result=ChartShareCard::render(value);QVERIFY2(result.available,qPrintable(result.reason));
    const auto text=captions(result);
    QVERIFY2(!QRegularExpression(QStringLiteral("https?://|\\[[0-9]+\\]|(?:stock|coin):")).match(text).hasMatch(),qPrintable(text));
    QVERIFY(text.contains(code));
    for(const auto* detail:{"계산 종료:","표시 범위:","원천 시각:","원래 수신:","no-lookahead","미확정:"})
        QVERIFY2(!text.contains(detail),detail);
    for(const auto* literal:{"coin:","stock:"}) {
        const auto glyph=literalGlyphPatch(QString::fromLatin1(literal),FontRole::Body15,themeColors().text2,themeColors().bg);
        QVERIFY(!containsLiteralGlyph(result.image,QRect(48,65,1104,26),glyph));
    }
    const auto urlGlyph=literalGlyphPatch(QStringLiteral("https://"),FontRole::Caption12,themeColors().text3,themeColors().bg);
    QVERIFY(!containsLiteralGlyph(result.image,QRect(48,754,1104,116),urlGlyph));
    QCOMPARE(value.identity.key(),identity);QCOMPARE(value.source.canonicalUrl,url);
    QCOMPARE(value.source.receivedAtMs,received);QCOMPARE(value.calculatedThroughMs,completed);
    QCOMPARE(value.studies.scorecard.samples,samples);QCOMPARE(value.studies.scorecard.overlappingSignals,overlap);
    // Source clock remains validated internally but is not an image footer.
    value.source.sourceTimeMs=0;const auto withoutSourceClock=ChartShareCard::render(value);QVERIFY(withoutSourceClock.available);
    QCOMPARE(withoutSourceClock.publicCaptions,result.publicCaptions);QCOMPARE(pixels(withoutSourceClock.image),pixels(result.image));
    value.source.canonicalUrl+="&private=FAKE_SCOPE";const auto rejected=ChartShareCard::render(value);
    QVERIFY(!rejected.available);QVERIFY(rejected.image.isNull());
}
void ChartShareCardTest::largePriceKeepsItsExactDecimalDigits() {
    auto value=snapshot(30);const QString exact="107780000.123456789";
    for(auto& row:value.closed)row.open=row.high=row.low=row.close=exact;
    value.studies=keefiChartStudies(value.closed,value.intervalMs);const auto result=ChartShareCard::render(value);
    QVERIFY(result.available);QVERIFY(captions(result).contains("107,780,000.123456789 KRW"));
    QCOMPARE(value.closed.last().close,exact);
    MovementColorPolicy colors;const auto glyph=literalGlyphPatch("107,780,000.123456789",FontRole::Value22,
        colors.foreground(MovementDirection::Flat),themeColors().bg);
    QVERIFY(containsLiteralGlyph(result.image,QRect(48,98,1104,32),glyph));
}
void ChartShareCardTest::themeChangesPublicInkWithoutChangingSourceOrCaptions() {
    KeefiTheme::instance()->setSettings(nullptr);const auto value=snapshot();
    KeefiTheme::instance()->setMode(ThemeMode::Light);const auto light=ChartShareCard::render(value);QVERIFY(light.available);
    QCOMPARE(light.image.pixelColor(0,0),QColor("#FFFFFF"));
    KeefiTheme::instance()->setMode(ThemeMode::Dark);const auto dark=ChartShareCard::render(value);QVERIFY(dark.available);
    QCOMPARE(dark.image.pixelColor(0,0),QColor("#17171C"));
    QVERIFY(pixels(light.image)!=pixels(dark.image));QCOMPARE(light.publicCaptions,dark.publicCaptions);
    QCOMPARE(value.identity.key(),QString("coin:upbit:KRW-SYN"));
}
void ChartShareCardTest::risingCandlesStayHollowAndFallingCandlesStayFilled_data() {
    QTest::addColumn<bool>("dark");QTest::addColumn<bool>("korean");
    QTest::newRow("light-green-red")<<false<<false;QTest::newRow("dark-green-red")<<true<<false;
    QTest::newRow("light-korean")<<false<<true;QTest::newRow("dark-korean")<<true<<true;
}
void ChartShareCardTest::risingCandlesStayHollowAndFallingCandlesStayFilled() {
    QFETCH(bool,dark);QFETCH(bool,korean);KeefiTheme::instance()->setSettings(nullptr);
    KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);auto value=candleStyleSnapshot();
    value.colors=korean?MovementScheme::KoreanRedBlue:MovementScheme::GreenRed;MovementColorPolicy colors;colors.setScheme(value.colors);
    const auto result=ChartShareCard::render(value);QVERIFY2(result.available,qPrintable(result.reason));QCOMPARE(result.image.size(),QSize(1200,900));
    const auto rise=colors.fill(MovementDirection::Up),fall=colors.fill(MovementDirection::Down),background=themeColors().surface;
    const int risingRun=longestInkRunAt(result.image,331,rise,fall,background),fallingRun=longestInkRunAt(result.image,331,fall,rise,background);
    QVERIFY2(risingRun>0,"An outlined rising body must remain visible");
    QVERIFY2(risingRun<=3,qPrintable(QString("A hollow rising body has only its 1px edge at the middle; observed a solid %1px run").arg(risingRun)));
    QVERIFY2(fallingRun>=12,qPrintable(QString("A falling body must keep its filled interior; observed only %1px").arg(fallingRun)));
    QVERIFY(saveF05Render(result.image,QString(dark?"dark-":"light-")+QString(korean?"korean-candles":"green-red-candles")));
}
void ChartShareCardTest::pullbackZoneUsesDiagonalSixPixelHatching_data(){
    QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;
}
void ChartShareCardTest::pullbackZoneUsesDiagonalSixPixelHatching(){
    QFETCH(bool,dark);KeefiTheme::instance()->setSettings(nullptr);KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    auto value=snapshot(21);QVERIFY2(value.studies.pullback.available,"The completed public 90-low/120-high fixture must produce a real support zone");
    value.visiblePublicLayers={"pullback"};const auto result=ChartShareCard::render(value);QVERIFY2(result.available,qPrintable(result.reason));
    const auto hatchInk=QColor::fromRgb(KeefiTheme::blend(themeColors().accent,themeColors().surface,.6).rgb());
    int hatchPixels=0,diagonalPairs=0,expectedIntervals=0,shortIntervals=0;const double horizontalInterval=6*std::sqrt(2.0);
    for(int y=0;y<result.image.height();++y){int previousStart=-1;bool previousInk=false;
        for(int x=0;x<result.image.width();++x){const bool ink=result.image.pixelColor(x,y)==hatchInk;
            if(ink){++hatchPixels;if(x+1<result.image.width()&&y>0&&result.image.pixelColor(x+1,y-1)==hatchInk)++diagonalPairs;}
            if(ink&&!previousInk){if(previousStart>=0){const int distance=x-previousStart;
                if(std::abs(distance-horizontalInterval)<=1)++expectedIntervals;else if(distance<horizontalInterval-1)++shortIntervals;}previousStart=x;}
            previousInk=ink;}
    }
    QVERIFY2(hatchPixels>50,"The exported support zone must contain 60% accent strokes rather than a flat translucent fill");
    QVERIFY2(expectedIntervals>20&&expectedIntervals>4*shortIntervals,"Hatch strokes must be six pixels apart perpendicular to their 45-degree direction");
    QVERIFY2(diagonalPairs>hatchPixels*.7,"The actual support-zone strokes must follow a 45-degree diagonal");
    QVERIFY(saveF05Render(result.image,QString(dark?"dark-":"light-")+"pullback-zone"));
}
void ChartShareCardTest::mixedCaptionsKeepNumberAndPriceRoles_data(){
    QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;
}
void ChartShareCardTest::mixedCaptionsKeepNumberAndPriceRoles(){
    QFETCH(bool,dark);KeefiTheme::instance()->setSettings(nullptr);KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
    auto value=snapshot();value.source.sourceTimeMs=Start;const auto result=ChartShareCard::render(value);QVERIFY2(result.available,qPrintable(result.reason));
    QVERIFY(!captions(result).contains(QStringLiteral("원천 시각:")));
    QVERIFY(captions(result).contains(QStringLiteral("최근 1번 중 1번 올랐어요")));
    QCOMPARE(value.source.sourceTimeMs,Start);QCOMPARE(value.studies.scorecard.meanReturnPercent,10.0);
    // Match small literal glyphs prescribed by the public font roles rather
    // than a whole-image golden or the renderer's private text helper.
    const auto code=literalGlyphPatch(QStringLiteral("SYN/KRW"),FontRole::Number11,themeColors().text2,themeColors().bg);
    QVERIFY2(containsLiteralGlyph(result.image,QRect(48,65,1104,26),code),"The public instrument code must retain Departure Mono11 after evidence footer removal");
    MovementColorPolicy colors;const auto priceInk=colors.foreground(MovementDirection::Up);
    const auto price=literalGlyphPatch(QStringLiteral("110"),FontRole::Value22,priceInk,themeColors().bg);
    const auto currency=literalGlyphPatch(QStringLiteral("KRW"),FontRole::Number11,priceInk,themeColors().bg);
    QVERIFY2(containsLiteralGlyph(result.image,QRect(48,98,700,32),price),"The shared card's existing main price must retain its Value22 glyphs");
    QVERIFY2(containsLiteralGlyph(result.image,QRect(48,98,700,32),currency),"The currency beside the main price must use the smaller Number11 code role");
    QCOMPARE(value.identity.key(),QStringLiteral("coin:upbit:KRW-SYN"));QCOMPARE(value.closed.last().close,QStringLiteral("110"));
    QVERIFY(saveF05Render(result.image,QString(dark?"dark-":"light-")+"mixed-captions"));
}
void ChartShareCardTest::publicCandlesAndScorecardRenderIntoANewDeterministicImage() {
    const auto value=snapshot();QVERIFY(value.studies.available);QVERIFY(value.studies.scorecard.available);QCOMPARE(value.studies.scorecard.samples,1);
    const auto first=ChartShareCard::render(value),second=ChartShareCard::render(value);QVERIFY2(first.available,qPrintable(first.reason));QVERIFY(!first.image.isNull());
    QCOMPARE(first.image.size(),QSize(1200,900));QCOMPARE(first.image.devicePixelRatio(),qreal(1));QVERIFY(first.image.textKeys().isEmpty());QCOMPARE(pixels(first.image),pixels(second.image));QCOMPARE(first.publicCaptions,second.publicCaptions);
    const auto text=captions(first);QVERIFY(text.contains("SYN/KRW"));QVERIFY(!text.contains(value.identity.key()));
    QVERIFY(text.contains("최근 1번 중 1번 올랐어요"));QVERIFY(text.contains("계좌 금액·수량 제외 · 로컬 저장"));QVERIFY(text.contains("시장 거래량"));
    QCOMPARE(value.studies.scorecard.horizonMs,qint64(3600000));QCOMPARE(value.studies.scorecard.meanReturnPercent,10.0);
    QVERIFY(value.studies.scorecard.costsExcluded);QCOMPARE(value.studies.scorecard.outcomes.size(),1);
}
void ChartShareCardTest::arbitraryReasonsAndLabelsNeverEnterTheImageOrCaptions() {
    auto value=snapshot(21);QVERIFY(value.studies.pullback.available);QVERIFY(!value.studies.pullback.evidence.isEmpty());const auto before=ChartShareCard::render(value);QVERIFY(before.available);
    value.studies.reason="FAKE_PRIVATE_ACCOUNT_A";value.studies.pullback.reason="FAKE_PRIVATE_AVERAGE_A";value.studies.similarity.reason="FAKE_PRIVATE_QUANTITY_A";value.studies.scorecard.reason="FAKE_PRIVATE_SCOPE_A";
    for(auto& evidence:value.studies.pullback.evidence)evidence.label="FAKE_PRIVATE_ALIAS_A";
    const auto after=ChartShareCard::render(value);QVERIFY(after.available);QCOMPARE(pixels(after.image),pixels(before.image));QCOMPARE(after.publicCaptions,before.publicCaptions);QVERIFY(!captions(after).contains("FAKE_PRIVATE"));QVERIFY(after.image.textKeys().isEmpty());
}
void ChartShareCardTest::invalidPublicInputRejectsBeforeRendering_data() {
    QTest::addColumn<QString>("fault");for(const auto* value:{"mask-off","size-small","size-large","identity","currency","timezone","url-private","url-token","url-userinfo","url-other-symbol","receipt-future","source-future","price-LF","price-leadingzero","bad-OHLC","duplicate-row","forming-in-closed","unknown-layer","too-many"})QTest::newRow(value)<<QString(value);
}
void ChartShareCardTest::invalidPublicInputRejectsBeforeRendering() {
    QFETCH(QString,fault);auto value=snapshot();ChartShareOptions options;
    if(fault=="mask-off")options.maskPersonalValues=false;if(fault=="size-small")options.outputSize={ChartShareCard::MinWidth-1,900};if(fault=="size-large")options.outputSize={ChartShareCard::MaxDimension+1,900};
    if(fault=="identity")value.identity.source="UNKNOWN";if(fault=="currency")value.currency="USD";if(fault=="timezone")value.displayTimeZoneId="FAKE/Timezone";
    if(fault=="url-private")value.source.canonicalUrl="https://api.upbit.com/v1/accounts";if(fault=="url-token")value.source.canonicalUrl+="&token=FAKE_SECRET";
    if(fault=="url-userinfo")value.source.canonicalUrl.replace("https://","https://FAKE_ACCOUNT@");if(fault=="url-other-symbol")value.source.canonicalUrl=MarketCandles::request("coin:upbit:KRW-OTHER",Minute).url.toString();
    if(fault=="receipt-future")value.source.receivedAtMs=value.asOfMs+1;if(fault=="source-future")value.source.sourceTimeMs=value.source.receivedAtMs+1;
    if(fault=="price-LF")value.closed.last().close+="\n";if(fault=="price-leadingzero")value.closed.last().open="0110";if(fault=="bad-OHLC")value.closed.last().high="1";
    if(fault=="duplicate-row")value.closed.last().startsAtMs=value.closed[value.closed.size()-2].startsAtMs;
    if(fault=="forming-in-closed"){auto row=value.closed.last();row.startsAtMs=value.asOfMs;value.closed.append(row);}
    if(fault=="unknown-layer")value.visiblePublicLayers.insert("FAKE_PRIVATE_AVERAGE");if(fault=="too-many"){const auto row=value.closed.last();while(value.closed.size()<=ChartShareCard::MaxClosedBars)value.closed.append(row);}
    const auto result=ChartShareCard::render(value,options);QVERIFY(!result.available);QVERIFY(!result.reason.isEmpty());QVERIFY(!result.reason.contains("FAKE"));QVERIFY(result.image.isNull());QVERIFY(result.publicCaptions.isEmpty());
}
void ChartShareCardTest::suppliedFutureStudyCannotBeAttachedToAnEarlierPrefix_data() {
    QTest::addColumn<QString>("fault");for(const auto* value:{"observations","study-time","calculated-through","future-outcome","score-count","score-mean","viewport-gap","future-suffix"})QTest::newRow(value)<<QString(value);
}
void ChartShareCardTest::suppliedFutureStudyCannotBeAttachedToAnEarlierPrefix() {
    QFETCH(QString,fault);auto value=snapshot();
    if(fault=="observations")++value.studies.observations;if(fault=="study-time")value.studies.lastCompletedAtMs+=Minute;if(fault=="calculated-through")++value.calculatedThroughMs;
    if(fault=="future-outcome")value.studies.scorecard.outcomes.first().evaluationAtMs=value.asOfMs+Minute;
    if(fault=="score-count")++value.studies.scorecard.samples;if(fault=="score-mean")value.studies.scorecard.meanReturnPercent+=100;
    if(fault=="viewport-gap")value.viewport.toMs-=Minute/2;if(fault=="future-suffix"){auto row=value.closed.last();row.startsAtMs+=Minute;value.closed.append(row);}
    const auto result=ChartShareCard::render(value);QVERIFY(!result.available);QVERIFY(!result.reason.isEmpty());QVERIFY(result.image.isNull());
}
void ChartShareCardTest::sourceClockZeroStaleAndUnavailableRemainExplicit() {
    auto value=snapshot();value.availability=ChartShareAvailability::Stale;const auto stale=ChartShareCard::render(value);QVERIFY(stale.available);
    QVERIFY(!captions(stale).contains("원천 시각"));QVERIFY(captions(stale).contains("지연"));QCOMPARE(value.source.sourceTimeMs,qint64(0));QVERIFY(!captions(stale).contains("최신 보장"));
    value.availability=ChartShareAvailability::Partial;const auto partial=ChartShareCard::render(value);QVERIFY(partial.available);QVERIFY(captions(partial).contains("확인 필요"));
}
void ChartShareCardTest::noHistoryNeverInventsAPriceOrScorecard() {
    auto value=snapshot();value.closed.clear();value.studies={};value.viewport={};value.calculatedThroughMs=0;value.availability=ChartShareAvailability::Waiting;
    const auto result=ChartShareCard::render(value);QVERIFY(result.available);QVERIFY(captions(result).contains("확인 필요"));QVERIFY(!captions(result).contains("최근 "));QVERIFY(!captions(result).contains("110 KRW"));
}
void ChartShareCardTest::unsupportedLegacyDailySourceCannotBeShared() {
    for(const auto* host:{"finance.yahoo.com","uk.finance.yahoo.com"}){
        auto value=stockSnapshot();
        // The removed public daily provider occupied ordinal 3. An unsupported
        // legacy value must fail closed even when its candles look valid.
        value.source.provider=static_cast<ChartShareSource>(3);value.source.delayed=true;
        value.source.canonicalUrl=QString("https://%1/quote/AAPL/history/").arg(host);
        const auto result=ChartShareCard::render(value);
        QVERIFY(!result.available);QVERIFY(result.image.isNull());QVERIFY(result.publicCaptions.isEmpty());
    }
}
void ChartShareCardTest::stockWithoutCompletionProofCannotBeShared() {
    auto value=stockSnapshot();value.calculatedThroughMs=0;value.studies={};
    for(auto& row:value.stockTimeline->rows){row.completesAtMs=0;row.eligible=false;}
    const auto result=ChartShareCard::render(value);
    QVERIFY(!result.available);QVERIFY(result.image.isNull());QVERIFY(result.publicCaptions.isEmpty());
}
void ChartShareCardTest::tossUsesTheActualCanonicalSourceRequest() {
    auto value=stockSnapshot();QCOMPARE(value.source.canonicalUrl,StockChartSource::request(StockChartRead::Candles,value.identity,Day).url.toString());
    const auto result=ChartShareCard::render(value);QVERIFY2(result.available,qPrintable(result.reason));QCOMPARE(value.source.adjustment,StockAdjustment::SourceAdjusted);QVERIFY(captions(result).contains("AAPL"));
    value.source.canonicalUrl="https://openapi.tossinvest.com/api/v1/candles";const auto rejected=ChartShareCard::render(value);QVERIFY(!rejected.available);QVERIFY(rejected.image.isNull());
}
void ChartShareCardTest::publicDrawingsAreExplicitOptInOnly() {
    auto value=snapshot();const auto baseline=ChartShareCard::render(value);QVERIFY(baseline.available);
    value.drawings={{ChartDrawingKind::Horizontal,{{value.viewport.fromMs,"103"}}}};
    const auto masked=ChartShareCard::render(value);QVERIFY(masked.available);QCOMPARE(pixels(masked.image),pixels(baseline.image));QCOMPARE(masked.publicCaptions,baseline.publicCaptions);
    ChartShareOptions enabled;enabled.includePublicDrawings=true;const auto drawn=ChartShareCard::render(value,enabled);QVERIFY(drawn.available);QVERIFY(pixels(drawn.image)!=pixels(baseline.image));
    value.drawings.first().anchors.first().atMs=value.closed.last().startsAtMs+2*Minute;const auto bad=ChartShareCard::render(value,enabled);QVERIFY(!bad.available);QVERIFY(bad.image.isNull());
    auto larger=snapshot();larger.intervalMs=15*Minute;
    for(int i=0;i<larger.closed.size();++i){larger.closed[i].startsAtMs=Start+i*larger.intervalMs;larger.closed[i].lastStoredTickMs=larger.closed[i].startsAtMs+larger.intervalMs-1;}
    larger.calculatedThroughMs=larger.closed.last().startsAtMs+larger.intervalMs;larger.asOfMs=larger.calculatedThroughMs+1000;larger.source.receivedAtMs=larger.calculatedThroughMs+500;
    larger.source.canonicalUrl=MarketCandles::request(larger.identity.key(),larger.intervalMs).url.toString();larger.studies=keefiChartStudies(larger.closed,larger.intervalMs);
    larger.viewport={larger.closed[21].startsAtMs,larger.closed.last().startsAtMs};const auto withoutFraction=ChartShareCard::render(larger,enabled);QVERIFY(withoutFraction.available);
    // A saved actual 1m anchor keeps its original UTC inside a later 15m parent.
    larger.drawings={{ChartDrawingKind::Horizontal,{{larger.viewport.fromMs+Minute,"103"}}}};
    const auto fraction=ChartShareCard::render(larger,enabled);QVERIFY2(fraction.available,qPrintable(fraction.reason));QVERIFY(pixels(fraction.image)!=pixels(withoutFraction.image));
}
void ChartShareCardTest::fibAndZoneDrawingsRenderAndHiddenDrawingsOmitFromShareSnapshot() {
    auto value=snapshot();const auto baseline=ChartShareCard::render(value);QVERIFY(baseline.available);
    ChartShareOptions enabled;enabled.includePublicDrawings=true;

    auto retVal=value;retVal.drawings={{ChartDrawingKind::FibRetracement,{{value.viewport.fromMs+2*Minute,"95"},{value.viewport.fromMs+12*Minute,"108"}}}};
    const auto retRender=ChartShareCard::render(retVal,enabled);
    QVERIFY2(retRender.available,qPrintable(retRender.reason));
    QVERIFY(pixels(retRender.image)!=pixels(baseline.image));
    auto badRet=retVal;badRet.drawings.first().anchors[1].price="95.0";
    QVERIFY(!ChartShareCard::render(badRet,enabled).available);

    auto ext2Val=value;ext2Val.drawings={{ChartDrawingKind::FibExtension,{{value.viewport.fromMs+2*Minute,"96"},{value.viewport.fromMs+10*Minute,"104"}}}};
    const auto ext2Render=ChartShareCard::render(ext2Val,enabled);
    QVERIFY2(ext2Render.available,qPrintable(ext2Render.reason));
    QVERIFY(pixels(ext2Render.image)!=pixels(baseline.image));
    QVERIFY(pixels(ext2Render.image)!=pixels(retRender.image));

    auto ext3Val=value;ext3Val.drawings={{ChartDrawingKind::FibExtension,{{value.viewport.fromMs+2*Minute,"96"},{value.viewport.fromMs+10*Minute,"104"},{value.viewport.fromMs+14*Minute,"98"}}}};
    const auto ext3Render=ChartShareCard::render(ext3Val,enabled);
    QVERIFY2(ext3Render.available,qPrintable(ext3Render.reason));
    QVERIFY(pixels(ext3Render.image)!=pixels(baseline.image));
    QVERIFY(pixels(ext3Render.image)!=pixels(ext2Render.image));
    auto badExt=ext2Val;badExt.drawings.first().anchors={badExt.drawings.first().anchors.first()};
    QVERIFY(!ChartShareCard::render(badExt,enabled).available);

    auto zoneVal=value;zoneVal.drawings={{ChartDrawingKind::Zone,{{value.viewport.fromMs+4*Minute,"97"},{value.viewport.fromMs+18*Minute,"107"}}}};
    const auto zoneRender=ChartShareCard::render(zoneVal,enabled);
    QVERIFY2(zoneRender.available,qPrintable(zoneRender.reason));
    QVERIFY(pixels(zoneRender.image)!=pixels(baseline.image));
    QVERIFY(pixels(zoneRender.image)!=pixels(retRender.image));
    auto sameTimeZone=zoneVal;sameTimeZone.drawings.first().anchors[1].atMs=sameTimeZone.drawings.first().anchors[0].atMs;
    QVERIFY(!ChartShareCard::render(sameTimeZone,enabled).available);
    auto samePriceZone=zoneVal;samePriceZone.drawings.first().anchors[1].price="97.00";
    QVERIFY(!ChartShareCard::render(samePriceZone,enabled).available);

    // MarketCandlesView::shareSnapshot skips hidden drawings (visible == false).
    class StubTransport final:public TradingTransport{
      public:
        quint64 token=0;
        void send(quint64 t,const RequestSpec&)override{token=t;}
        void abort()override{}
    } transport;
    const qint64 viewNow=Start+10*Minute;
    MarketCandles source(&transport,std::unique_ptr<PublicCandleSocket>{},[viewNow]{return viewNow;});
    ChartDrawings drawings;ChartStudyPreferences preferences;preferences.markIntroSeen();
    MarketCandlesView view(&source);view.setIndicatorPreferences(&preferences);view.setDrawingModel(&drawings);
    view.setDrawingPublicContext(true);view.setSelection("coin:upbit:KRW-SYN",Minute);view.resize(1050,950);view.show();
    QVERIFY(transport.token);
    QJsonArray array;
    for(int i=0;i<6;++i){
        const qint64 at=Start+i*Minute;
        array.append(QJsonObject{{"market","KRW-SYN"},{"candle_date_time_utc",QDateTime::fromMSecsSinceEpoch(at,QTimeZone::utc()).toString("yyyy-MM-ddTHH:mm:ss")},
            {"timestamp",QString::number(at+1000)},{"unit","1"},{"opening_price","100"},{"high_price","105"},{"low_price","95"},{"trade_price","102"},{"candle_acc_trade_volume","2"}});
    }
    HttpResult res;res.status=200;res.body=QJsonDocument(array).toJson();
    emit transport.finished(transport.token,res);
    QCOMPARE(source.series().candles.size(),6);
    ChartDrawingFrame frame;frame.identity=drawings.context().identity;frame.currency="KRW";frame.intervalMs=Minute;frame.asOfMs=viewNow;frame.contextGeneration=drawings.contextGeneration();
    for(const auto& row:source.series().candles)frame.bars.append({row,row.startsAtMs+Minute,true,!frame.bars.isEmpty()});
    const auto visibleFib=drawings.create(ChartDrawingKind::FibRetracement,{{Start,"96"},{Start+3*Minute,"104"}},frame);
    const auto hiddenZone=drawings.create(ChartDrawingKind::Zone,{{Start+Minute,"97"},{Start+4*Minute,"103"}},frame);
    QVERIFY(!visibleFib.isEmpty()&&!hiddenZone.isEmpty());
    QCOMPARE(view.shareSnapshot(enabled,viewNow).drawings.size(),2);
    QVERIFY(drawings.setDrawingVisible(hiddenZone,false));
    const auto snap=view.shareSnapshot(enabled,viewNow);
    QCOMPARE(snap.drawings.size(),1);
    QCOMPARE(snap.drawings.first().kind,ChartDrawingKind::FibRetracement);
    QVERIFY(ChartShareCard::render(snap,enabled).available);
}
void ChartShareCardTest::movementSchemeChangesPublicInkButKeepsDirectionAndFacts() {
    auto value=snapshot();const auto first=ChartShareCard::render(value);QVERIFY(first.available);value.colors=MovementScheme::KoreanRedBlue;const auto second=ChartShareCard::render(value);QVERIFY(second.available);
    QVERIFY(pixels(first.image)!=pixels(second.image));QCOMPARE(first.publicCaptions,second.publicCaptions);QVERIFY(captions(second).contains("▲"));
}
void ChartShareCardTest::localPngHasNoPrivateOrArbitraryMetadata() {
    QTemporaryDir directory;QVERIFY(directory.isValid());const auto value=snapshot();const auto one=directory.filePath("one.png"),two=directory.filePath("two.png");QString error;
    QVERIFY2(ChartShareCard::savePng(value,{},one,&error),qPrintable(error));QVERIFY(ChartShareCard::savePng(value,{},two,&error));const auto raw=read(one);QVERIFY(!raw.isEmpty());QCOMPARE(raw,read(two));
    QImageReader reader(one);QVERIFY(reader.canRead());QVERIFY(reader.textKeys().isEmpty());const auto image=reader.read();QVERIFY(!image.isNull());QVERIFY(image.textKeys().isEmpty());
    const auto names=chunks(raw);QVERIFY(names.contains("IHDR"));QVERIFY(names.contains("IDAT"));QVERIFY(names.contains("IEND"));
    const QStringList allowed{"IHDR","PLTE","IDAT","IEND","pHYs","gAMA","cHRM","sRGB","tRNS"};for(const auto& name:names)QVERIFY2(allowed.contains(name),qPrintable(name));
    QVERIFY(!raw.contains("FAKE_PRIVATE"));QVERIFY(!raw.contains(directory.path().toUtf8()));
}
void ChartShareCardTest::invalidSaveDoesNotReplaceAnExistingFile() {
    QTemporaryDir directory;const auto path=directory.filePath("existing.png");QFile old(path);QVERIFY(old.open(QIODevice::WriteOnly));QCOMPARE(old.write("ORIGINAL_SENTINEL"),qint64(17));old.close();
    ChartShareOptions invalid;invalid.maskPersonalValues=false;QString error;QVERIFY(!ChartShareCard::savePng(snapshot(),invalid,path,&error));QVERIFY(!error.isEmpty());QCOMPARE(read(path),QByteArray("ORIGINAL_SENTINEL"));
}
void ChartShareCardTest::saveAcceptsOnlyExplicitAbsolutePngPaths_data() {
    QTest::addColumn<QString>("path");QTest::newRow("relative")<<QString("relative.png");QTest::newRow("https")<<QString("https://example.invalid/card.png");QTest::newRow("file-url")<<QString("file:///tmp/card.png");QTest::newRow("wrong-extension")<<QString("WRONG_EXTENSION");
}
void ChartShareCardTest::saveAcceptsOnlyExplicitAbsolutePngPaths() {
    QFETCH(QString,path);QTemporaryDir directory;if(path=="WRONG_EXTENSION")path=directory.filePath("card.txt");QString error;QVERIFY(!ChartShareCard::savePng(snapshot(),{},path,&error));QVERIFY(!error.isEmpty());
}
QTEST_MAIN(ChartShareCardTest)
#include "tst_chart_share_card.moc"
