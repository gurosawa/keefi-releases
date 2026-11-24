#include "keefi/ChartStudySearch.h"
#include "keefi/ChartStudyPreferences.h"
#include <QTest>
using namespace keefi;

class ChartStudySearchTest final : public QObject {
    Q_OBJECT
private slots:
    void keysIgnoreSpacingWidthCaseAndSeparators();
    void choseongUsesCompatibilityConsonants();
    void initialsAndQuestionsExplainMatches();
    void initialsIgnoreSpacingAndFullwidthSeparators();
    void strongestMatchWinsAndEmptyQueryHasNoReason();
    void correctedAliasesStayPrecise();
    void englishNamesPreserveEveryLegacyKoreanName();
};

void ChartStudySearchTest::keysIgnoreSpacingWidthCaseAndSeparators() {
    QCOMPARE(chartStudySearchKey(QStringLiteral("상대 강도")), chartStudySearchKey(QStringLiteral("상대강도")));
    QCOMPARE(chartStudySearchKey(QStringLiteral("ＲＳＩ")), QStringLiteral("rsi"));
    QCOMPARE(chartStudySearchKey(QStringLiteral(" R S I\t·-/?\n")), QStringLiteral("rsi"));
    QCOMPARE(chartStudySearchKey(QStringLiteral("많이 올랐나?")), QStringLiteral("많이올랐나"));
}

void ChartStudySearchTest::choseongUsesCompatibilityConsonants() {
    QCOMPARE(chartStudyChoseong(QStringLiteral("볼린저")), QStringLiteral("ㅂㄹㅈ"));
    QCOMPARE(chartStudyChoseong(QStringLiteral("이동평균")), QStringLiteral("ㅇㄷㅍㄱ"));
    QCOMPARE(chartStudyChoseong(QStringLiteral("쌍")), QStringLiteral("ㅆ"));
    QCOMPARE(chartStudyChoseong(QStringLiteral("BTC 대비")), QStringLiteral("BTC ㄷㅂ"));
}

void ChartStudySearchTest::initialsAndQuestionsExplainMatches() {
    const auto initials = matchChartStudy("bb", QStringLiteral("ㅂㄹㅈ"));
    QVERIFY(initials.matched);
    QCOMPARE(initials.rank, 2);
    QVERIFY(initials.reason.contains(QStringLiteral("초성")));
    QVERIFY(matchChartStudy("ma", QStringLiteral("ㅇㄷㅍㄱ")).matched);
    QVERIFY(!matchChartStudy("bb", QStringLiteral("ㅂ")).matched);
    const auto question = matchChartStudy("rsi", QStringLiteral("많이 올랐나?"));
    QVERIFY(question.matched);
    QCOMPARE(question.rank, 1);
    QVERIFY(question.reason.contains(QStringLiteral("많이 올랐")));
    for (const auto* id : {"rsi", "relative_strength"}) {
        const auto alias = matchChartStudy(id, QStringLiteral("상대 강도"));
        QVERIFY(alias.matched);
        QCOMPARE(alias.rank, 2);
        QVERIFY(alias.reason.contains(QStringLiteral("다른 이름")));
    }
    const auto profile = matchChartStudy("low_volume", QStringLiteral("매물대"));
    QVERIFY(profile.matched);
    QVERIFY(profile.reason.contains(QStringLiteral("드문")));
    QVERIFY(matchChartStudy("bb", QStringLiteral("흔들림")).matched);
    QVERIFY(matchChartStudy("squeeze", QStringLiteral("흔들림")).matched);
}

void ChartStudySearchTest::strongestMatchWinsAndEmptyQueryHasNoReason() {
    const auto name = matchChartStudy("bb", QStringLiteral("Bollinger Bands"));
    QVERIFY(name.matched);
    QCOMPARE(name.rank, 3);
    QVERIFY(name.reason.isEmpty());
    const auto alias = matchChartStudy("bb", QStringLiteral("밴드"));
    QVERIFY(alias.matched);
    QCOMPARE(alias.rank, 2);
    QVERIFY(alias.reason.startsWith(QStringLiteral("다른 이름")));
    ChartStudyPreferences prefs;
    auto ids = prefs.supportedIds("coin") + prefs.supportedIds("stock");
    ids.removeDuplicates();
    for (const auto& id : ids) {
        const auto empty = matchChartStudy(id, {});
        QVERIFY2(empty.matched, qPrintable(id));
        QVERIFY2(empty.reason.isEmpty(), qPrintable(id));
        const auto miss = matchChartStudy(id, QStringLiteral("없는검색문자열"));
        QVERIFY2(!miss.matched, qPrintable(id));
        QCOMPARE(miss.rank, 0);
        QVERIFY(miss.reason.isEmpty());
    }
}

void ChartStudySearchTest::initialsIgnoreSpacingAndFullwidthSeparators() {
    for (const auto& query : {QStringLiteral("ㅂ ㄹ ㅈ"), QStringLiteral("ㅂㄹㅈ？")}) {
        const auto match = matchChartStudy("bb", query);
        QVERIFY2(match.matched, qPrintable(query));
        QCOMPARE(match.rank, 2);
        QVERIFY(match.reason.contains(QStringLiteral("초성")));
    }
}

void ChartStudySearchTest::correctedAliasesStayPrecise() {
    QVERIFY(!matchChartStudy("pullback", QStringLiteral("추세")).matched);
    QVERIFY(!matchChartStudy("contract", QStringLiteral("펀딩")).matched);
    QVERIFY(!matchChartStudy("contract", "Funding").matched);
    QVERIFY(!matchChartStudy("contract", "Basis").matched);
    QVERIFY(matchChartStudy("sentiment", QStringLiteral("펀딩")).matched);
    QVERIFY(matchChartStudy("sentiment", "Funding").matched);
    ChartStudyPreferences prefs;
    auto ids = prefs.supportedIds("coin") + prefs.supportedIds("stock");
    ids.removeDuplicates();
    for (const auto& id : ids) {
        QCOMPARE(matchChartStudy(id, "Bollinger").matched, id == "bb");
        QCOMPARE(matchChartStudy(id, "RSI").matched, id == "rsi");
    }
}

void ChartStudySearchTest::englishNamesPreserveEveryLegacyKoreanName() {
    const QMap<QString,QString> legacy{{"pullback","눌림존"},{"whales","큰손·반전"},{"similarity","닮은꼴 · 과거 비교"},{"cloud","추세 구름"},{"volume","거래량"},{"stock_flow","수급·거래량"},{"ma","이동평균"},{"bb","볼린저"},{"rsi","RSI"},{"macd","MACD"},{"levels","레벨선"},{"cvd","체결차"},{"oi","미결제약정"},{"contract","계약 흐름"},{"sentiment","선물 정보"},{"absorption","흡수 근사"},{"sweep","쓸고 복귀"},{"squeeze","변동성 압축"},{"adx","ADX"},{"breakout","돌파"},{"low_volume","빈 가격대"},{"strong","강한 반전"},{"trend","여러 시간봉 추세"},{"relative_strength","BTC 대비 흐름"}};
    for(auto it=legacy.cbegin();it!=legacy.cend();++it){QVERIFY2(matchChartStudy(it.key(),it.value()).matched,qPrintable(it.key()));QVERIFY2(matchChartStudy(it.key(),it.key()).matched,qPrintable(it.key()));}
    const auto korean=matchChartStudy("bb",QStringLiteral("볼린저"));QCOMPARE(korean.rank,3);QVERIFY(korean.reason.contains(QStringLiteral("다른 이름")));
    QVERIFY(matchChartStudy("volume",QStringLiteral("거래량")).rank>matchChartStudy("low_volume",QStringLiteral("거래량")).rank);
    QVERIFY(matchChartStudy("cvd",QStringLiteral("금액별 체결차")).matched);
}

QTEST_APPLESS_MAIN(ChartStudySearchTest)
#include "tst_chart_study_search.moc"
