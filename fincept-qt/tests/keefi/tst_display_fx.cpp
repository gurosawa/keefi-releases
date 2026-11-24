#include "keefi/PortfolioDisplayFx.h"
#include <QTest>

using namespace keefi;
namespace {
constexpr qint64 Now = 1793000000000;
AssetQuote coinbase(const QString& price = QStringLiteral("0.998"), qint64 sourceTime = Now - 1000, qint64 receiptTime = Now - 500) {
    return {"USDT", "USD", price, {"Synthetic Coinbase", "https://api.exchange.coinbase.com/products/USDT-USD/ticker",
        "public:coinbase:USDT-USD", sourceTime, receiptTime, true}};
}
}
class DisplayFxTest final : public QObject {
    Q_OBJECT
  private slots:
    void passiveObservationSurvivesDemandStopUntilOriginalExpiry() {
        PortfolioDisplayFx cache;
        const auto quote = coinbase();
        cache.observeCoinbase(quote, Now);
        QCOMPARE(cache.quotes(Now), QList<AssetQuote>{quote});
        // Stopping market demand sends no observation and must not erase this public leg.
        QCOMPARE(cache.quotes(Now + 179000), QList<AssetQuote>{quote});
        QVERIFY(cache.quotes(Now + 179001).isEmpty());
        QCOMPARE(quote.evidence.sourceTimeMs, Now - 1000);
        QCOMPARE(quote.evidence.receivedAtMs, Now - 500);
    }
    void failureRequiresAStrictlyNewerObservationToRecover() {
        PortfolioDisplayFx cache;
        const auto original = coinbase();
        cache.observeCoinbase(original, Now);
        cache.invalidate();
        QVERIFY(cache.quotes(Now).isEmpty());
        cache.observeCoinbase(original, Now + 1);
        QVERIFY2(cache.quotes(Now + 1).isEmpty(), "An unchanged source tick cannot revive an actively failed leg");
        cache.observeCoinbase(coinbase("0.997", Now - 2000, Now), Now + 1);
        QVERIFY(cache.quotes(Now + 1).isEmpty());
        const auto newer = coinbase("0.999", Now - 999, Now);
        cache.observeCoinbase(newer, Now + 1);
        QCOMPARE(cache.quotes(Now + 1), QList<AssetQuote>{newer});
    }
    void duplicatesKeepOriginalClocksAndConflictsCannotRevive() {
        PortfolioDisplayFx cache;
        const auto original = coinbase();
        cache.observeCoinbase(original, Now);
        auto duplicate = original;
        duplicate.price = "0.9980";
        duplicate.evidence.receivedAtMs = Now;
        cache.observeCoinbase(duplicate, Now);
        QCOMPARE(cache.quotes(Now), QList<AssetQuote>{original});
        cache.observeCoinbase(coinbase("0.997", Now - 2000, Now), Now);
        QCOMPARE(cache.quotes(Now), QList<AssetQuote>{original});
        QVERIFY(cache.quotes(Now + 179001).isEmpty());

        auto conflicting = duplicate;
        conflicting.price = "1.002";
        cache.observeCoinbase(conflicting, Now);
        QVERIFY2(cache.quotes(Now).isEmpty(), "Same-source-time conflicting prices must be unavailable");
        cache.observeCoinbase(original, Now);
        QVERIFY(cache.quotes(Now).isEmpty());
        const auto newer = coinbase("1.001", Now - 999, Now);
        cache.observeCoinbase(newer, Now);
        QCOMPARE(cache.quotes(Now), QList<AssetQuote>{newer});
        cache.observeCoinbase(conflicting, Now);
        QCOMPARE(cache.quotes(Now), QList<AssetQuote>{newer});
    }
    void invalidObservationsCannotEnterOrAdvanceTheWatermark_data() {
        QTest::addColumn<QString>("fault");
        for (const auto* fault : {"empty", "wrong-base", "wrong-quote", "zero-price", "negative-price", "invalid-price", "exponent-price",
            "long-price", "incomplete", "empty-source", "empty-url", "wrong-url", "url-query", "private-scope", "wrong-public-scope",
            "source-zero", "receipt-zero", "source-future", "receipt-future", "source-stale", "receipt-stale", "source-after-receipt", "as-of-zero"})
            QTest::newRow(fault) << QString::fromLatin1(fault);
    }
    void invalidObservationsCannotEnterOrAdvanceTheWatermark() {
        QFETCH(QString, fault);
        PortfolioDisplayFx cache;
        auto quote = coinbase();
        auto asOf = Now;
        if (fault == "empty") quote = {};
        if (fault == "wrong-base") quote.baseAsset = "BTC";
        if (fault == "wrong-quote") quote.quoteAsset = "KRW";
        if (fault == "zero-price") quote.price = "0";
        if (fault == "negative-price") quote.price = "-1";
        if (fault == "invalid-price") quote.price = "NaN";
        if (fault == "exponent-price") quote.price = "1e0";
        if (fault == "long-price") quote.price = QString(81, '1');
        if (fault == "incomplete") quote.evidence.complete = false;
        if (fault == "empty-source") quote.evidence.source = " ";
        if (fault == "empty-url") quote.evidence.url = {};
        if (fault == "wrong-url") quote.evidence.url = "https://example.invalid/products/USDT-USD/ticker";
        if (fault == "url-query") quote.evidence.url += "?extra=1";
        if (fault == "private-scope") quote.evidence.scope = "private:synthetic";
        if (fault == "wrong-public-scope") quote.evidence.scope = "public:upbit:KRW-USDT";
        if (fault == "source-zero") quote.evidence.sourceTimeMs = 0;
        if (fault == "receipt-zero") quote.evidence.receivedAtMs = 0;
        if (fault == "source-future") quote.evidence.sourceTimeMs = Now + 1;
        if (fault == "receipt-future") quote.evidence.receivedAtMs = Now + 1;
        if (fault == "source-stale") quote.evidence.sourceTimeMs = Now - 180001;
        if (fault == "receipt-stale") quote.evidence.receivedAtMs = Now - 180001;
        if (fault == "source-after-receipt") quote.evidence.sourceTimeMs = Now - 499;
        if (fault == "as-of-zero") asOf = 0;
        const auto original = quote;
        cache.observeCoinbase(quote, asOf);
        QVERIFY(cache.quotes(Now).isEmpty());
        QCOMPARE(quote, original);
        const auto valid = coinbase();
        cache.observeCoinbase(valid, Now);
        QCOMPARE(cache.quotes(Now), QList<AssetQuote>{valid});
    }
    void emptyFailureInvalidatesButOldInvalidRepliesCannotEraseNewerData() {
        PortfolioDisplayFx cache;
        const auto original = coinbase();
        cache.observeCoinbase(original, Now);
        cache.observeCoinbase({}, Now);
        QVERIFY(cache.quotes(Now).isEmpty());
        cache.observeCoinbase(original, Now);
        QVERIFY(cache.quotes(Now).isEmpty());
        const auto newer = coinbase("1.001", Now - 999, Now);
        cache.observeCoinbase(newer, Now);
        auto oldInvalid = original;
        oldInvalid.evidence.complete = false;
        cache.observeCoinbase(oldInvalid, Now);
        QCOMPARE(cache.quotes(Now), QList<AssetQuote>{newer});
    }
};
QTEST_GUILESS_MAIN(DisplayFxTest)
#include "tst_display_fx.moc"
