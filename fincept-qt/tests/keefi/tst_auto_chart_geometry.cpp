#include "keefi/AutoChartGeometry.h"
#include "keefi/ChartDrawings.h"
#include <QTest>
#include <cmath>

using namespace keefi;

namespace {
constexpr qint64 Hour = 3600000;
constexpr qint64 Day = 86400000;
constexpr qint64 BaseAt = 1792713600000;

struct SimpleBar {
    double open, high, low, close;
    bool connectsPrevious = true;
    QString customHigh, customLow;
};

ChartDrawingFrame buildFrame(const QList<SimpleBar>& specs,
                             const WatchlistIdentity& identity = {"coin", "upbit", "KRW-SYN"},
                             const QString& currency = "KRW",
                             qint64 interval = Hour) {
    ChartDrawingFrame frame;
    frame.identity = identity;
    frame.currency = currency;
    frame.intervalMs = interval;
    for (int i = 0; i < specs.size(); ++i) {
        const auto& s = specs[i];
        const QString o = QString::number(s.open, 'f', 2);
        const QString h = s.customHigh.isEmpty() ? QString::number(s.high, 'f', 2) : s.customHigh;
        const QString l = s.customLow.isEmpty() ? QString::number(s.low, 'f', 2) : s.customLow;
        const QString c = QString::number(s.close, 'f', 2);
        ChartDrawingBar bar;
        const qint64 at = BaseAt + qint64(i) * (s.connectsPrevious || i == 0 ? interval : interval * 2);
        // Keep monotonically increasing starts; for coin keep strict interval spacing unless testing stock gaps.
        bar.candle = {BaseAt + qint64(i) * interval, 0, o, h, l, c, "10"};
        bar.completesAtMs = bar.candle.startsAtMs + interval;
        bar.sourceCompleted = true;
        bar.connectsPrevious = (i > 0) && s.connectsPrevious;
        frame.bars.append(bar);
        Q_UNUSED(at);
    }
    if (!frame.bars.isEmpty())
        frame.asOfMs = frame.bars.last().completesAtMs + 1000;
    return frame;
}

const AutoChartCandidate* findCandidate(const AutoChartResult& res, const QString& key) {
    for (const auto& c : res.candidates)
        if (c.key == key)
            return &c;
    return nullptr;
}
} // namespace

class AutoChartGeometryTest final : public QObject {
    Q_OBJECT
  private slots:
    void invalidShortAndZeroAtrFramesFailWithExactReasons();
    void zigzagPivotsConfirmAlternatingExtremesAndSkipReversalOnNewHighBar();
    void disconnectedStockBarUsesOnlyHighMinusLowForTrueRange();
    void fibCandidatesSupportTwoAndThreePointWavesInBothDirections();
    void horizontalLevelsClusterWithinToleranceAndRankTopThree();
    void sidewaysZoneSelectsLatestConsolidationAndExcludesEqualPrices();
    void trendlineTracksTouchesRejectsTwoPiercesAndCollectsWickSweeps();
    void resultsAreDeterministicAndPreserveRawBarTimestampsAndStrings();
    void candidatesApplyCleanlyIntoChartDrawingsBatch();
};

void AutoChartGeometryTest::invalidShortAndZeroAtrFramesFailWithExactReasons() {
    QList<SimpleBar> twenty(20, {100, 102, 98, 101, true, {}, {}});
    auto invalid = buildFrame(twenty);
    invalid.bars[0].sourceCompleted = false;
    auto r1 = AutoChartGeometry::analyze(invalid);
    QVERIFY(!r1.available);
    QCOMPARE(r1.reason, QStringLiteral("완료 봉 자료를 확인할 수 없어요"));

    auto shortFrame = buildFrame(twenty);
    auto r2 = AutoChartGeometry::analyze(shortFrame);
    QVERIFY(!r2.available);
    QCOMPARE(r2.reason, QStringLiteral("완료 봉이 21개 이상 필요해요"));

    QList<SimpleBar> flat(21, {100, 100, 100, 100, true, {}, {}});
    auto r3 = AutoChartGeometry::analyze(buildFrame(flat));
    QVERIFY(!r3.available);
    QCOMPARE(r3.reason, QStringLiteral("가격 변동이 없어 계산할 수 없어요"));

    // No shape candidate (available = true with explicit reason): 21 alternating small 1-bar waves < theta
    QList<SimpleBar> smallOsc;
    for (int i = 0; i < 21; ++i)
        smallOsc.append({100 + i * 3.0, 101 + i * 3.0, 99 + i * 3.0, 100.5 + i * 3.0, true, {}, {}});
    // Steady trend step +3 each bar => monotonic high & low, zero zigzag reversals, range > 2*ATR so no zone either
    auto r4 = AutoChartGeometry::analyze(buildFrame(smallOsc));
    QVERIFY(r4.available);
    QVERIFY(r4.candidates.isEmpty());
    QCOMPARE(r4.reason, QStringLiteral("찾은 도형이 없어요"));
}

void AutoChartGeometryTest::zigzagPivotsConfirmAlternatingExtremesAndSkipReversalOnNewHighBar() {
    // 25 bars, constant TR = 2.0 on bars 1..20? Keep range small (~2) on calm bars so ATR20 is ~4 and theta ~8.
    // Craft bars with predictable TR: bars 0..24
    QList<SimpleBar> bars;
    for (int i = 0; i < 25; ++i)
        bars.append({100, 102, 98, 100, true, {}, {}}); // TR=4 everywhere baseline
    // Wave moves inside bars: keep TR of each bar modest or known:
    // Let's make bars 5..24 have TR=4 except wave steps so ATR20 is around 4..5 -> theta ~ 8..10.
    // Actually compute ATR20 exact from bars we specify:
    bars = {
        {100,102, 98,100}, // 0
        {100,102, 98,100}, // 1
        {100,102, 98,100}, // 2
        {100,102, 98,100}, // 3
        {100,102, 98,100}, // 4
        {100,102, 96, 97}, // 5: low extreme = 96
        { 97,102, 97,101}, // 6
        {101,108,100,107}, // 7: upMove 108-96=12 => confirms bar 5 LOW(96), dir=+1, candBar=7(108)
        {107,112,106,111}, // 8: high=112 > 108 => candBar moves to 8
        {111,116,102,103}, // 9: NEW HIGH 116 > 112 AND range 14 >= theta, MUST NOT reverse on same bar! candBar moves to 9(116)
        {103,115,103,104}, // 10: high 115 <= 116, 116-103 = 13 >= theta => confirms bar 9 HIGH(116)! dir=-1, candBar=10(103)
        {104,104, 92, 93}, // 11: low=92 < 103 => candBar moves to 11(92)
        { 93,105, 93,104}, // 12: high 105-92 = 13 >= theta => confirms bar 11 LOW(92)! dir=+1, candBar=12(105)
        {104,106,103,105}, // 13
        {105,107,104,106}, // 14
        {106,108,105,107}, // 15
        {107,109,106,108}, // 16
        {108,110,107,109}, // 17
        {109,110,107,108}, // 18
        {108,109,106,107}, // 19
        {107,109,106,108}, // 20
        {108,109,106,107}, // 21
        {107,108,105,106}, // 22
        {106,108,105,106}, // 23
        {106,107,104,105}, // 24
    };
    const auto frame = buildFrame(bars);
    const auto res = AutoChartGeometry::analyze(frame);
    QVERIFY(res.available);
    // Verify theta is small enough (<11) and big enough (>6) for designed swings
    const double theta = AutoChartGeometry::ZigzagAtr * res.atr;
    QVERIFY2(theta > 6.0 && theta < 12.0, qPrintable(QString::number(theta)));

    QCOMPARE(res.pivots.size(), 3);
    QCOMPARE(res.pivots[0].bar, 5);  QCOMPARE(res.pivots[0].high, false); QCOMPARE(res.pivots[0].price, QString("96.00"));
    QCOMPARE(res.pivots[1].bar, 9);  QCOMPARE(res.pivots[1].high, true);  QCOMPARE(res.pivots[1].price, QString("116.00"));
    QCOMPARE(res.pivots[2].bar, 11); QCOMPARE(res.pivots[2].high, false); QCOMPARE(res.pivots[2].price, QString("92.00"));
}

void AutoChartGeometryTest::disconnectedStockBarUsesOnlyHighMinusLowForTrueRange() {
    QList<SimpleBar> connected, disconnected;
    for (int i = 0; i < 21; ++i) {
        SimpleBar b{100, 102, 98, 100, true, {}, {}}; // normal TR = 4
        if (i == 10) {
            // Huge overnight gap up after bar 9 close=100: bar 10 open=120, high=122, low=118, close=120.
            // high-low = 4, while |high - prevClose(100)| = 22!
            b = {120, 122, 118, 120, true, {}, {}};
        } else if (i > 10) {
            b = {120, 122, 118, 120, true, {}, {}};
        }
        connected.append(b);
        if (i == 10) b.connectsPrevious = false;
        disconnected.append(b);
    }
    const WatchlistIdentity stock{"stock", "KR", "005930"};
    const auto resConn = AutoChartGeometry::analyze(buildFrame(connected, stock, "KRW", Day));
    const auto resDisc = AutoChartGeometry::analyze(buildFrame(disconnected, stock, "KRW", Day));
    QVERIFY(resConn.available && resDisc.available);
    // Connected ATR20: (19*4 + 22)/20 = 4.9; Disconnected ATR20: (20*4)/20 = 4.0
    QVERIFY(std::abs(resConn.atr - 4.9) <= 1e-6);
    QVERIFY(std::abs(resDisc.atr - 4.0) <= 1e-6);
}

void AutoChartGeometryTest::fibCandidatesSupportTwoAndThreePointWavesInBothDirections() {
    // 1) k == 2 uptrend wave -> 2-pt retracement and 2-pt extension
    QList<SimpleBar> k2Bars(22, {92, 93, 91, 92, true, {}, {}});
    k2Bars[0]  = { 81,  82, 80,  81}; // Low 80
    k2Bars[1]  = { 81,  82, 80,  81};
    k2Bars[2]  = { 81,  83, 81,  82};
    k2Bars[3]  = { 82,  86, 82,  85};
    for (int i = 4; i <= 9; ++i) k2Bars[i] = {85 + (i - 4) * 5.0, 86 + (i - 4) * 5.0, 85 + (i - 4) * 5.0, 86 + (i - 4) * 5.0};
    k2Bars[10] = {112, 120, 112, 118}; // High 120 (+40 confirms LOW 80)
    k2Bars[11] = {118, 118, 110, 111};
    k2Bars[12] = {111, 112, 102, 103};
    k2Bars[13] = {103, 104,  96,  97};
    k2Bars[14] = { 97,  98,  90,  92}; // -30 confirms bar 10 HIGH(120) -> 2 pivots: (LOW:80, HIGH:120)
    for (int i = 15; i < 22; ++i) k2Bars[i] = {92, 93, 91, 92};

    const auto k2Res = AutoChartGeometry::analyze(buildFrame(k2Bars));
    QVERIFY(k2Res.available);
    QCOMPARE(k2Res.pivots.size(), 2);
    const auto* k2Ret = findCandidate(k2Res, "fib-retracement");
    const auto* k2Ext = findCandidate(k2Res, "fib-extension");
    QVERIFY(k2Ret && k2Ext);
    QCOMPARE(k2Ret->anchors.size(), 2);
    QCOMPARE(k2Ext->anchors.size(), 2);

    const auto checkRet = [&](const AutoChartCandidate* cand, double A, double B) {
        ChartDrawing d; d.kind = cand->kind; d.anchors = cand->anchors;
        const auto levels = ChartDrawings::fibLevels(d);
        QCOMPARE(levels.size(), 7);
        for (const auto& lvl : levels) {
            const double expected = B - lvl.ratio * (B - A);
            QVERIFY2(std::abs(lvl.price - expected) <= 1e-6, qPrintable(QString("%1 vs %2").arg(lvl.price).arg(expected)));
        }
    };
    const auto checkExt3 = [&](const AutoChartCandidate* cand, double A, double B, double C) {
        ChartDrawing d; d.kind = cand->kind; d.anchors = cand->anchors;
        const auto levels = ChartDrawings::fibLevels(d);
        for (const auto& lvl : levels) {
            const double expected = C + lvl.ratio * (B - A);
            QVERIFY(expected > 0);
            QVERIFY2(std::abs(lvl.price - expected) <= 1e-6, qPrintable(QString("%1 vs %2").arg(lvl.price).arg(expected)));
        }
    };
    checkRet(k2Ret, 80.0, 120.0);

    // 2) k >= 3 uptrend wave -> 3-pt extension using C
    auto k3Up = k2Bars;
    k3Up[14] = { 97,  98,  95,  96}; // Low 95 confirms High 120
    k3Up[15] = { 96, 105,  96, 104};
    k3Up[16] = {104, 112, 104, 111};
    k3Up[17] = {111, 118, 111, 117};
    k3Up[18] = {117, 125, 117, 124}; // High 125 reverses +30 -> confirms bar 14 Low 95 -> pivots [80, 120, 95]
    for (int i = 19; i < 22; ++i) k3Up[i] = {124, 124.5, 123.5, 124};
    const auto k3UpRes = AutoChartGeometry::analyze(buildFrame(k3Up));
    QCOMPARE(k3UpRes.pivots.size(), 3);
    const auto* upRet = findCandidate(k3UpRes, "fib-retracement");
    const auto* upExt = findCandidate(k3UpRes, "fib-extension");
    QVERIFY(upRet && upExt);
    QCOMPARE(upExt->anchors.size(), 3);
    checkRet(upRet, 80.0, 120.0);
    checkExt3(upExt, 80.0, 120.0, 95.0);

    // 3) k >= 3 downtrend wave -> 3-pt extension descending & dropping <= 0 if applicable
    QList<SimpleBar> k3Down(22, {152, 153, 151, 152});
    k3Down[0]  = {238, 240, 237, 239};
    k3Down[1]  = {238, 240, 237, 239};
    k3Down[2]  = {238, 240, 237, 239};
    k3Down[3]  = {239, 240, 235, 236}; // High 240
    for (int i = 4; i <= 7; ++i) k3Down[i] = {230 - (i - 4) * 15.0, 232 - (i - 4) * 15.0, 220 - (i - 4) * 15.0, 222 - (i - 4) * 15.0};
    k3Down[8]  = {170, 171, 160, 162}; // Low 160 -> confirms High(240)
    for (int i = 9; i <= 12; ++i) k3Down[i] = {165 + (i - 9) * 10.0, 172 + (i - 9) * 10.0, 164 + (i - 9) * 10.0, 170 + (i - 9) * 10.0};
    k3Down[13] = {198, 205, 198, 202}; // High 205 -> confirms Low(160)
    for (int i = 14; i <= 17; ++i) k3Down[i] = {195 - (i - 14) * 10.0, 196 - (i - 14) * 10.0, 185 - (i - 14) * 10.0, 186 - (i - 14) * 10.0};
    k3Down[18] = {160, 161, 150, 152}; // Low 150 -> confirms High(205) -> pivots [240, 160, 205]
    for (int i = 19; i < 22; ++i) k3Down[i] = {152, 153, 151, 152};
    const auto k3DownRes = AutoChartGeometry::analyze(buildFrame(k3Down));
    QCOMPARE(k3DownRes.pivots.size(), 3);
    const auto* dnRet = findCandidate(k3DownRes, "fib-retracement");
    const auto* dnExt = findCandidate(k3DownRes, "fib-extension");
    QVERIFY(dnRet && dnExt);
    QCOMPARE(dnExt->anchors.size(), 3);
    checkRet(dnRet, 240.0, 160.0);
    checkExt3(dnExt, 240.0, 160.0, 205.0);
}

void AutoChartGeometryTest::horizontalLevelsClusterWithinToleranceAndRankTopThree() {
    // Craft 4 distinct level clusters (40 bars so ATR20 is clean ~2.0, tol = 1.0):
    // Cluster A near 90: touches=2 (far from lastClose=100)
    // Cluster B near 100: touches=3 (best touches)
    // Cluster C near 112: touches=2 (distance to 100 = 12)
    // Cluster D near 95: touches=2 (distance to 100 = 5, closer than 112 and 90!)
    // Cluster E near 105: touches=2 (distance to 100 = 5, tied with D=95 -> higher price wins! -> 105 over 95)
    // Result should keep B(touches=3), E(105, touches=2, dist=5), D(95, touches=2, dist=5) -> sorted price desc: 105 -> 100 -> 95!
    // No: let's keep 4 clusters: B(touches=3, ~100), A(touches=2, ~85 dist 15), C(touches=2, ~105 dist 5), D(touches=2, ~95 dist 5).
    // Expected kept: B(100), C(105), D(95), ordered descending: horizontal:1 (~105), horizontal:2 (~100), horizontal:3 (~95).
    QList<SimpleBar> bars(44, {100, 100.5, 99.5, 100});
    // Low/High alternating waves with >= 4.0 swing so every extremum confirms a pivot while ATR20 stays small (~2.0 -> theta~4).
    bars[2]  = {100,101, 85.0, 86}; // L 85.0 (Cluster A1)
    bars[4]  = { 86,120.0, 86,118}; // H 120
    bars[6]  = {118,118, 85.6, 86}; // L 85.6 (Cluster A2, bar 6 latest in A)
    bars[8]  = { 86,121.0, 86,120}; // H 121 (unused single unless >=121) -> Wait: 120 and 121 are within 1.0! Raise H waves so highs stay spread apart!
    // Use distinct high peaks so highs don't accidentally cluster >= 2!
    bars[4]  = { 86,118.0, 86,117};
    bars[8]  = { 86,125.0, 86,124};
    bars[10] = {124,124, 95.0, 96}; // L 95.0 (Cluster D1)
    bars[12] = { 96,132.0, 96,130};
    bars[14] = {130,130, 95.4, 96}; // L 95.4 (Cluster D2, dist from 100 is 4.6)
    bars[16] = { 96,139.0, 96,138};
    bars[18] = {138,138,100.0,101}; // L 100.0 (Cluster B1)
    bars[20] = {101,106.0,101,105}; // Wait: bars >= 24 decide ATR20! Let's check last 20 bars = indices 24..43!
    // Put ALL quiet bars on indices 24..43 so TR is tiny OR put waves before 24 AND Cluster waves also tested cleanly!
    // Wait: if bars 24..43 have high/low waves of size 8 every 2 bars, ATR20 is ~8 (theta=16, tol=4).
    // Even simpler: just put 23 bars BEFORE index 20 so bars 20..43 have constant TR=2.0 and NO wild swings? Wait — after bar 20 we only need pivots AFTER or BEFORE bar 24!
    // Confirming bar 24 is already inside ATR20! So run ALL pivot clusters on bars 1..22 (keeping TR=1 on 23..42 -> wait: ATR20 only reads the LAST 20 bars!).
    // If last 20 bars (indices 24..43) ALL have TR = 2.0 (range 99..101), ATR20 = 2.0 exact, theta = 4.0 exact, tol = 1.0 exact!
    bars = QList<SimpleBar>(44, {100, 100.5, 99.5, 100}); // Reset
    for (int i = 24; i < 44; ++i) bars[i] = {100, 101.0, 99.0, 100.0}; // last 20 bars exact TR = 2.0 -> ATR20 = 2.0, theta = 4.0, tol = 1.0!
    // Now on indices 1..22, construct alternating pivots with >= 4.0 swings, where last pivot settles cleanly on bar 23!
    bars[0]  = {100,114.0, 99.5,100};
    bars[1]  = {100,101, 85.0, 86}; // L 85.0 (Cluster A1)
    bars[2]  = { 86,120.0, 86,119}; // H 120
    bars[3]  = {119,119, 85.5, 86}; // L 85.5 (Cluster A2 -> bar 3, price 85.50, touches=2)
    bars[4]  = { 86,126.0, 86,125}; // H 126
    bars[5]  = {125,125, 95.0, 96}; // L 95.0 (Cluster D1)
    bars[6]  = { 96,133.0, 96,132}; // H 133
    bars[7]  = {132,132, 95.0, 96}; // L 95.0 (Cluster D2 -> bar 7, price 95.00, touches=2, dist to close 100 = 5.0)
    bars[8]  = { 96,140.0, 96,139}; // H 140
    bars[9]  = {139,139,105.0,106}; // L 105.0 (Cluster C1)
    bars[10] = {106,148.0,106,147}; // H 148
    bars[11] = {147,147,105.0,106}; // L 105.0 (Cluster C2 -> bar 11, price 105.00, touches=2, dist to close 100 = 5.0, higher price than D!)
    bars[12] = {106,155.0,106,154}; // H 155
    bars[13] = {154,154,100.0,101}; // L 100.0 (Cluster B1)
    bars[14] = {101,162.0,101,161}; // H 162
    bars[15] = {161,161,100.2,101}; // L 100.2 (Cluster B2)
    bars[16] = {101,169.0,101,168}; // H 169
    bars[17] = {168,168,100.4,101}; // L 100.4 (Cluster B3 -> bar 17, price 100.40, touches=3)
    bars[18] = {101,176.0,101,175}; // H 176 -> confirmed by bar 19 drop to 101!
    bars[19] = {175,175,112.0,113}; // low 112 confirms H176; bar 20 low 100 wouldn't collide, wait set bars[19] low=112, bars[20] high=120, wait 112+8 >= 120 confirms L112? No keep bars 19..23 gently descending so H176 confirms at 19 and candLow sits at 23 without hitting 95/100/105/85!
    bars[19] = {175,175,112.0,112}; // drops >= 4 from 176 => confirms H176, candLow=19(112)
    bars[20] = {112,113,111.0,111};
    bars[21] = {111,112,110.0,110};
    bars[22] = {110,111,109.0,109};
    bars[23] = {109,110,100.0,100.5}; // candLow 23(100), then bars 24..43 stay 99..101 => candLow moves to 99 at 24 and stays >=99, <=101 (< theta=4), never confirming!

    const auto frame = buildFrame(bars);
    const auto res = AutoChartGeometry::analyze(frame);
    QVERIFY(res.available);
    QVERIFY(std::abs(res.atr - 2.0) <= 1e-6);

    const auto* h1 = findCandidate(res, "horizontal:1");
    const auto* h2 = findCandidate(res, "horizontal:2");
    const auto* h3 = findCandidate(res, "horizontal:3");
    const auto* h4 = findCandidate(res, "horizontal:4");
    QVERIFY(h1 && h2 && h3);
    QVERIFY(!h4);

    // Top 3 clusters sorted high-to-low: C(105.00), B(100.40, touches=3), D(95.00)
    QCOMPARE(h1->anchors.first().price, QString("105.00"));
    QCOMPARE(h1->touches, 2);
    QCOMPARE(h1->anchors.first().atMs, frame.bars[11].candle.startsAtMs);

    QCOMPARE(h2->anchors.first().price, QString("100.40"));
    QCOMPARE(h2->touches, 3);
    QCOMPARE(h2->anchors.first().atMs, frame.bars[17].candle.startsAtMs);

    QCOMPARE(h3->anchors.first().price, QString("95.00"));
    QCOMPARE(h3->touches, 2);
    QCOMPARE(h3->anchors.first().atMs, frame.bars[7].candle.startsAtMs);
}

void AutoChartGeometryTest::sidewaysZoneSelectsLatestConsolidationAndExcludesEqualPrices() {
    // 25 bars: bars 0..14 wide range (TR = 10 -> ATR20 approx (11*10 + 9*2)/20 = 6.4 -> ZoneRangeAtr*ATR = 12.8!)
    // Actually make bars 0..14 wide range TR = 10 -> at end=24, last 20 bars = 5..24:
    // indices 5..14 (10 bars) have TR = 10; indices 15..24 (10 bars) have narrow range [98.0 .. 103.0] (maxH - minL = 5.0 <= 12.8!).
    // bar 14 has high=120, low=90 => extending start from 24 backward stops at bar 15 (start=15, end=24, length=10 >= 8)!
    QList<SimpleBar> bars;
    for (int i = 0; i < 14; ++i)
        bars.append({105, 110, 100, 105}); // TR = 10 on i=5..13
    bars.append({105, 130, 95, 105}); // bar 14 spike range 35 (> ZoneRangeAtr*ATR20) breaks backward consolidation at index 15!
    for (int i = 15; i < 25; ++i) {
        if (i == 16) bars.append({100, 104.5, 99.0, 101}); // zone max high = 104.50
        else if (i == 21) bars.append({100, 102.0, 97.5, 100}); // zone min low = 97.50
        else bars.append({100, 102.0, 99.0, 100});
    }
    const auto frame = buildFrame(bars);
    const auto res = AutoChartGeometry::analyze(frame);
    QVERIFY(res.available);
    const auto* zone = findCandidate(res, "zone");
    QVERIFY(zone);
    QCOMPARE(zone->anchors.size(), 2);
    QCOMPARE(zone->anchors[0].atMs, frame.bars[15].candle.startsAtMs);
    QCOMPARE(zone->anchors[0].price, QString("104.50"));
    QCOMPARE(zone->anchors[1].atMs, frame.bars[24].candle.startsAtMs);
    QCOMPARE(zone->anchors[1].price, QString("97.50"));

    // Equal prices inside the last >= 8 bars -> no zone candidate created
    auto flatEnd = bars;
    for (int i = 15; i < 25; ++i) flatEnd[i] = {100, 100, 100, 100};
    const auto flatRes = AutoChartGeometry::analyze(buildFrame(flatEnd));
    QVERIFY(flatRes.available);
    QVERIFY(!findCandidate(flatRes, "zone"));
}

void AutoChartGeometryTest::trendlineTracksTouchesRejectsTwoPiercesAndCollectsWickSweeps() {
    // Design ascending low pivots on bar indices 2 (100.0), 6 (104.0), 10 (108.0) -> y(x) = 98 + x.
    // Smoothly connect intermediate bars inside the waves so no accidental extra pivots form.
    // Bars 20..39 stay calm around 150 (TR=2.0 on 21..39) => ATR20 <= 3.3, theta <= 6.6, tau <= 0.83.
    QList<SimpleBar> bars(40, {110, 111.0, 109.0, 110.0});
    bars[0]  = {110,111.0,109.0,110.0};
    bars[1]  = {110,110.5,105.0,106.0};
    bars[2]  = {106,107.0,100.0,102.0}; // Low pivot 1 @ bar 2 = 100.0
    bars[3]  = {102,110.0,102.0,109.0};
    bars[4]  = {109,118.0,109.0,116.0}; // High peak @ bar 4 = 118.0
    bars[5]  = {116,116.5,109.0,110.0};
    bars[6]  = {110,110.5,104.0,105.0}; // Low pivot 2 @ bar 6 = 104.0
    bars[7]  = {105,118.0,105.0,117.0};
    bars[8]  = {117,131.0,117.0,130.0}; // High peak @ bar 8 = 131.0
    bars[9]  = {130,130.5,118.0,119.0};
    bars[10] = {119,120.0,108.0,110.0}; // Low pivot 3 @ bar 10 = 108.0
    bars[11] = {110,117.0,110.0,116.0};
    bars[12] = {116,124.0,116.0,123.0}; // High peak @ bar 12 = 124.0
    bars[13] = {123,124.0,121.0,122.0};
    bars[14] = {122,124.0,121.0,122.0};
    bars[15] = {122,124.0,121.0,122.0};
    bars[16] = {122,124.0,121.0,122.0};
    bars[17] = {122,124.0,121.0,122.0};
    bars[18] = {122,122.5,112.0,118.0}; // bar 18: line(18)=116.0, Low=112.0 (<116-tau, away from (2,6,10) line chord), Close=118.0 => SWEEP!
    bars[19] = {118,125.0,118.0,124.0};
    for (int i = 20; i < 40; ++i) bars[i] = {150, 151.0, 149.0, 150.0};

    const auto frame = buildFrame(bars);
    const auto res = AutoChartGeometry::analyze(frame);
    QVERIFY(res.available);
    const auto* trend = findCandidate(res, "trend");
    QVERIFY(trend);
    QCOMPARE(trend->touches, 3);
    QCOMPARE(trend->anchors.size(), 2);
    QVERIFY(trend->sweepAtMs.contains(frame.bars[18].candle.startsAtMs));

    auto pierced = bars;
    pierced[15] = {112, 112.5, 109.0, 109.0};
    pierced[16] = {110, 111.0, 109.0, 110.0};
    pierced[18] = {122, 124.0, 121.0, 122.0};
    const auto piercedRes = AutoChartGeometry::analyze(buildFrame(pierced));
    QVERIFY(piercedRes.available);
    QVERIFY(!findCandidate(piercedRes, "trend"));
}

void AutoChartGeometryTest::resultsAreDeterministicAndPreserveRawBarTimestampsAndStrings() {
    QList<SimpleBar> bars(24, {100, 102, 98, 100});
    bars[3]  = {100, 101, 90, 91, true, {}, QStringLiteral("90.1250")};
    bars[9]  = { 91, 115, 91, 114, true, QStringLiteral("115.0500"), {}};
    bars[14] = {114, 114, 90.125, 91, true, {}, QStringLiteral("90.1250")};
    const auto frame = buildFrame(bars);
    const auto first = AutoChartGeometry::analyze(frame);
    const auto second = AutoChartGeometry::analyze(frame);
    QVERIFY(first.available);
    QCOMPARE(first.atr, second.atr);
    QCOMPARE(first.pivots.size(), second.pivots.size());
    QCOMPARE(first.candidates.size(), second.candidates.size());
    for (int i = 0; i < first.candidates.size(); ++i) {
        QCOMPARE(first.candidates[i].key, second.candidates[i].key);
        QCOMPARE(first.candidates[i].kind, second.candidates[i].kind);
        QCOMPARE(first.candidates[i].anchors, second.candidates[i].anchors);
        QCOMPARE(first.candidates[i].touches, second.candidates[i].touches);
        QCOMPARE(first.candidates[i].sweepAtMs, second.candidates[i].sweepAtMs);
    }

    bool sawRawFraction = false;
    for (const auto& cand : first.candidates) {
        for (const auto& anchor : cand.anchors) {
            bool matchedBar = false;
            for (const auto& bar : frame.bars) {
                if (bar.candle.startsAtMs == anchor.atMs) {
                    matchedBar = true;
                    QVERIFY(anchor.price == bar.candle.high || anchor.price == bar.candle.low);
                    if (anchor.price == QStringLiteral("90.1250") || anchor.price == QStringLiteral("115.0500"))
                        sawRawFraction = true;
                    break;
                }
            }
            QVERIFY(matchedBar);
        }
    }
    QVERIFY(sawRawFraction);
}

void AutoChartGeometryTest::candidatesApplyCleanlyIntoChartDrawingsBatch() {
    QList<SimpleBar> bars(40, {120, 121.0, 119.0, 120.0});
    for (int i = 20; i < 40; ++i) bars[i] = {150, 151.0, 149.0, 150.0};
    bars[1]  = {110,111.0,109.0,110.0};
    bars[2]  = {110,110.0,100.0,102.0};
    bars[4]  = {102,118.0,102.0,116.0};
    bars[6]  = {116,116.0,104.0,105.0};
    bars[8]  = {105,131.0,105.0,130.0};
    bars[10] = {130,130.0,108.0,110.0};
    bars[12] = {110,124.0,110.0,123.0};
    bars[18] = {122,122.5,114.5,118.0};
    bars[19] = {118,125.0,118.0,124.0};

    ChartDrawings model;
    QVERIFY(model.setContext({{"coin", "upbit", "KRW-SYN"}, "KRW", true}, Hour));
    auto frame = buildFrame(bars);
    frame.contextGeneration = model.contextGeneration();
    const auto res = AutoChartGeometry::analyze(frame);
    QVERIFY(res.available);
    QVERIFY(!res.candidates.isEmpty());

    QList<ChartDrawing> batch;
    for (const auto& cand : res.candidates)
        batch.append({{}, cand.kind, cand.anchors});
    QString error;
    QVERIFY2(model.applyBatch(batch, frame, &error), qPrintable(error));
    QCOMPARE(model.drawings().size(), batch.size());
}

QTEST_GUILESS_MAIN(AutoChartGeometryTest)
#include "tst_auto_chart_geometry.moc"
