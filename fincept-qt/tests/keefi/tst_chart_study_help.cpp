#include "keefi/ChartStudyHelp.h"
#include "keefi/ChartStudies.h"
#include "keefi/ChartStudyPreferences.h"
#include <QTest>
#include <QCoreApplication>
#include <QTranslator>
#include <cmath>
using namespace keefi;

class ChartStudyHelpTest final : public QObject {
    Q_OBJECT
private slots:
    void coreNamesHaveFivePublicExplanationLayers();
    void everySupportedStudyHasPlainFiveLayerHelp();
    void newDefinitionsUseImplementedThresholds();
    void similarityIsHistoricalOptionalAndDoesNotPromiseAmplitude();
    void similarityHelpMatchesEnabledDefault();
    void definitionsUseImplementedMaterialsAndThresholds();
    void scaledLogReturnsReallyMatchWithoutEqualPriceChange();
    void nameLookupTranslatesOnlyRequestedStudyAndTracksTranslatorChanges();
};

void ChartStudyHelpTest::nameLookupTranslatesOnlyRequestedStudyAndTracksTranslatorChanges() {
    int argc=1;char name[]="chart-study-name-test";char* argv[]{name,nullptr};
    QCoreApplication app(argc,argv);
    class Translator final:public QTranslator {
    public:
        mutable QStringList requests;
        QString prefix="first:";
        bool isEmpty()const override{return false;}
        QString translate(const char* context,const char* source,const char*,int)const override{
            if(QString::fromUtf8(context)!="QObject")return {};
            requests.append(QString::fromUtf8(source));return prefix+QString::fromUtf8(source);
        }
    } translator;
    QVERIFY(app.installTranslator(&translator));
    QCOMPARE(chartStudyName("cloud"),QStringLiteral("first:Trend Cloud"));
    // Translation is an external service: a single displayed name must not
    // request translations for every unrelated study on each market update.
    QCOMPARE(translator.requests,QStringList{QStringLiteral("Trend Cloud")});
    translator.requests.clear();translator.prefix="second:";
    QCOMPARE(chartStudyName("cloud"),QStringLiteral("second:Trend Cloud"));
    QCOMPARE(translator.requests,QStringList{QStringLiteral("Trend Cloud")});
    translator.requests.clear();
    QVERIFY(chartStudyName("unknown").isEmpty());
    QCOMPARE(chartStudyName("rsi"),QStringLiteral("RSI"));
    QVERIFY(translator.requests.isEmpty());
    QVERIFY(app.removeTranslator(&translator));
    QCOMPARE(chartStudyName("cloud"),QStringLiteral("Trend Cloud"));
}

void ChartStudyHelpTest::coreNamesHaveFivePublicExplanationLayers() {
    // Retain the original four-study assertions; full coverage is checked below.
    const ChartStudyPreferences preferences;
    for (const auto& id : QStringList{"cloud", "pullback", "whales", "similarity"}) {
        const auto value = chartStudyHelp(id);
        QVERIFY(value.has_value());
        QCOMPARE(value->id, id);
        QVERIFY(!value->title.isEmpty());
        for (const auto& section : {value->brief, value->inputs, value->reading, value->limitations, value->calculation})
            QVERIFY2(!section.isEmpty(), qPrintable(id));
        QVERIFY(value->markets.contains("coin"));
        QCOMPARE(value->markets.contains("stock"), preferences.supportedIds("stock").contains(id));
    }
    QVERIFY(!chartStudyHelp("unrecognized").has_value());
}

void ChartStudyHelpTest::everySupportedStudyHasPlainFiveLayerHelp() {
    const ChartStudyPreferences preferences;
    const auto coin = preferences.supportedIds("coin");
    const auto stock = preferences.supportedIds("stock");
    auto supported = coin + stock;
    supported.removeDuplicates();
    auto actual = chartStudyHelpIds();
    QCOMPARE(actual.size(), supported.size());
    QCOMPARE(actual.first(4), QStringList({"cloud", "pullback", "whales", "similarity"}));
    actual.sort();
    supported.sort();
    QCOMPARE(actual, supported);
    const QStringList banned{QStringLiteral("매수 신호"), QStringLiteral("매도 신호"),
        QStringLiteral("사세요"), QStringLiteral("파세요"), QStringLiteral("추천"),
        QStringLiteral("적중"), QStringLiteral("수익")};
    for (const auto& id : supported) {
        const auto value = chartStudyHelp(id);
        QVERIFY2(value.has_value(), qPrintable(id));
        QCOMPARE(value->id, id);
        QCOMPARE(value->title, chartStudyName(id));
        QVERIFY2(!value->title.isEmpty(), qPrintable(id));
        for (const auto& section : {value->brief, value->inputs, value->reading,
                                   value->limitations, value->calculation})
            QVERIFY2(!section.isEmpty(), qPrintable(id));
        QCOMPARE(value->markets.contains("coin"), coin.contains(id));
        QCOMPARE(value->markets.contains("stock"), stock.contains(id));
        QCOMPARE(value->markets.size(), int(coin.contains(id)) + int(stock.contains(id)));
        QCOMPARE(value->historicalOnly, id == "similarity");
        for (const auto& section : {value->brief, value->reading, value->limitations})
            for (const auto& word : banned)
                QVERIFY2(!section.contains(word), qPrintable(id + ": " + word));
    }
}

void ChartStudyHelpTest::newDefinitionsUseImplementedThresholds() {
    const QMap<QString, QStringList> calculationTokens{
        {"rsi", {"14"}}, {"macd", {"12", "26", "9"}}, {"bb", {"200", "201", "2"}},
        {"ma", {"21", "50", "100", "200", "201", QStringLiteral("4줄"), QStringLiteral("2~200")}},
        {"adx", {"14", "25", "20"}}, {"squeeze", {"1.5"}}, {"breakout", {"20"}},
        {"absorption", {"22", QStringLiteral("2배"), QStringLiteral("절반")}},
        {"cvd", {QStringLiteral("1,000만"), QStringLiteral("1억"), "0.2"}},
        {"relative_strength", {"24"}},
        {"low_volume", {QStringLiteral("20칸"), "1.25%", "96"}},
        {"levels", {"0.05%"}},
        {"trend", {QStringLiteral("1시간"), QStringLiteral("4시간"), QStringLiteral("1일"), "20"}},
        {"strong", {"2", "0.2"}}
    };
    for (auto it = calculationTokens.cbegin(); it != calculationTokens.cend(); ++it) {
        const auto value = chartStudyHelp(it.key());
        QVERIFY2(value.has_value(), qPrintable(it.key()));
        for (const auto& token : it.value())
            QVERIFY2(value->calculation.contains(token), qPrintable(it.key() + ": " + token));
    }
    const auto rsi = chartStudyHelp("rsi");
    QVERIFY(rsi.has_value());
    QVERIFY(rsi->reading.contains("55"));
    QVERIFY(rsi->reading.contains("45"));
    for (const auto& id : {"volume", "sentiment", "strong"}) {
        const auto value = chartStudyHelp(id);
        QVERIFY(value.has_value());
        QVERIFY2((value->reading + value->limitations).contains(QStringLiteral("현재 방향 요약에는 넣지 않아요")), id);
    }
}

void ChartStudyHelpTest::similarityIsHistoricalOptionalAndDoesNotPromiseAmplitude() {
    const auto value = chartStudyHelp("similarity");
    QVERIFY(value.has_value());
    QVERIFY(value->historicalOnly);
    QVERIFY(value->brief.contains(QStringLiteral("같은 종목")));
    QVERIFY(value->reading.contains(QStringLiteral("현재 방향")));
    QVERIFY(value->limitations.contains(QStringLiteral("상승폭")));
    QVERIFY(value->limitations.contains(QStringLiteral("예측")));
    QVERIFY(value->limitations.contains(QStringLiteral("성과")));
    QVERIFY(value->limitations.contains(QStringLiteral("모든 종목에 적용")));
    QVERIFY(value->calculation.contains(QString::number(ChartSimilarityMinimumCorrelation, 'f', 2)));
    QVERIFY(value->calculation.contains("200"));
    QVERIFY(value->calculation.contains("Pearson"));
    QVERIFY(value->calculation.contains(QStringLiteral("변동성")));
}

void ChartStudyHelpTest::similarityHelpMatchesEnabledDefault() {
    const ChartStudyPreferences preferences;
    QVERIFY(preferences.enabled("similarity"));
    const auto value = chartStudyHelp("similarity");
    QVERIFY(value.has_value());
    QVERIFY(!value->limitations.contains(QStringLiteral("꺼져 있고")));
    QVERIFY(value->limitations.contains(QStringLiteral("켜져")));
}

void ChartStudyHelpTest::definitionsUseImplementedMaterialsAndThresholds() {
    const auto cloud = chartStudyHelp("cloud"), pullback = chartStudyHelp("pullback"), whales = chartStudyHelp("whales");
    QVERIFY(cloud && pullback && whales);
    QVERIFY(cloud->calculation.contains("EMA 12"));
    QVERIFY(cloud->calculation.contains("EMA 26"));
    QVERIFY(cloud->reading.contains(QStringLiteral("빠른선")));
    QVERIFY(pullback->calculation.contains("ATR 20"));
    QVERIFY(pullback->calculation.contains("0.5"));
    QVERIFY(pullback->limitations.contains(QStringLiteral("확률")));
    QVERIFY(whales->inputs.contains(QStringLiteral("완료 1분")));
    QVERIFY(whales->calculation.contains("200"));
    QVERIFY(whales->calculation.contains("99"));
    QVERIFY(whales->limitations.contains(QStringLiteral("투자자")));
    QVERIFY(whales->limitations.contains(QStringLiteral("업비트")));
}

void ChartStudyHelpTest::scaledLogReturnsReallyMatchWithoutEqualPriceChange() {
    // A synthetic, deterministic calculation test, not performance evidence.
    constexpr qint64 base = 1728000000000, interval = 60000;
    QList<MarketCandle> rows;
    double historical = 100, following = 0, query = 100;
    for (int i = 0; i < 60; ++i) {
        double close;
        if (i < 20) {
            if (i) historical *= std::exp(.001 * i);
            close = historical;
            following = historical;
        } else if (i < 40) {
            following *= std::exp(-.01);
            close = following;
        } else {
            if (i > 40) query *= std::exp(.002 * (i - 40));
            close = query;
        }
        const auto price = QString::number(close, 'f', 12);
        rows.append({base + i * interval, base + i * interval + 1000,
                     price, QString::number(close * 1.01, 'f', 12),
                     QString::number(close * .99, 'f', 12), price, "10"});
    }
    const auto result = keefiChartStudies(rows, interval);
    QVERIFY(result.available);
    QVERIFY2(result.similarity.available, qPrintable(result.similarity.reason));
    QCOMPARE(result.similarity.samples, 1);
    const auto& path = result.similarity.paths.first();
    QVERIFY(std::abs(path.correlation - 1) < 1e-10);
    const auto pastChange = rows[19].close.toDouble() / rows[0].close.toDouble() - 1;
    const auto recentChange = rows[59].close.toDouble() / rows[40].close.toDouble() - 1;
    QVERIFY(std::abs(pastChange - std::expm1(.19)) < 1e-10);
    QVERIFY(std::abs(recentChange - std::expm1(.38)) < 1e-10);
    QVERIFY(recentChange > 2 * pastChange);
    QVERIFY(!result.similarity.volatilityNormalized);
    QVERIFY(path.points.last().price < rows.last().close.toDouble());
}
QTEST_APPLESS_MAIN(ChartStudyHelpTest)
#include "tst_chart_study_help.moc"
