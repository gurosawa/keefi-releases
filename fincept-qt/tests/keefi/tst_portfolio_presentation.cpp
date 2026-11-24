#include "keefi/PortfolioPresentation.h"
#include "keefi/PortfolioAssetModel.h"
#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QTest>
using namespace keefi;
class PortfolioPresentationTest:public QObject {
    Q_OBJECT
  private slots:
    void sameSnapshotIsSilentAndChangedDenominatorUpdatesOtherShares() {
        PortfolioAssetModel model(nullptr);
        QAbstractItemModelTester contracts(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        const auto grouped = [](const QString& symbol, const QString& value) {
            PortfolioHoldingView holding;
            holding.asset = symbol;
            holding.componentId = symbol;
            holding.connectionId = "synthetic";
            holding.value = value;
            holding.state = PortfolioValueState::Fresh;
            holding.presence = AssetPresence::Held;
            PortfolioGroupView group;
            group.identity = symbol;
            group.asset = symbol;
            group.freshTotal = value;
            group.holdings = {holding};
            group.presence = holding.presence;
            return group;
        };
        auto first = grouped("A", "150");
        const auto second = grouped("B", "50");
        model.setSnapshot({first,second}, {}, {}, "200", false);
        QPersistentModelIndex stable(model.index(1, 5));
        QCOMPARE(stable.data().toString(), QString("25.0%"));
        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
        model.setSnapshot({first,second}, {}, {}, "200", false);
        QCOMPARE(changes.size(), 0);
        first = grouped("A", "50");
        model.setSnapshot({second,first}, {}, {}, "100", false);
        QVERIFY(stable.isValid());
        QCOMPARE(stable.row(), 0);
        QCOMPARE(stable.data().toString(), QString("50.0%"));
        bool sawShare = false;
        for (const auto& signal : changes) {
            const auto from = signal[0].value<QModelIndex>();
            const auto to = signal[1].value<QModelIndex>();
            if (!from.parent().isValid() && from.row() <= stable.row() && to.row() >= stable.row() &&
                from.column() <= 5 && to.column() >= 5) sawShare = true;
        }
        QVERIFY(sawShare);
        model.clearPrivate();
        QVERIFY(!stable.isValid());
        QCOMPARE(model.rowCount(), 0);
    }
    void quantityProvenanceSurvivesCurrentAndRecordedViews_data(){
        QTest::addColumn<bool>("recorded");QTest::newRow("current")<<false;QTest::newRow("recorded")<<true;
    }
    void quantityProvenanceSurvivesCurrentAndRecordedViews(){
        QFETCH(bool,recorded);
        const qint64 at=1792600000000;
        AssetSnapshot snapshot;snapshot.id="FAKE_SNAPSHOT";snapshot.accountScope="FAKE_PRIVATE_SCOPE";snapshot.connectionId="FAKE_CONNECTION";
        snapshot.provider="binance";snapshot.market="spot";snapshot.environment="live";snapshot.timeMs=at;snapshot.balanceComplete=true;
        snapshot.evidence={"Binance 공식 잔고","https://api.binance.com/api/v3/account",snapshot.accountScope,at-20,at-10,true};
        AssetComponent quantity;quantity.id="coin";quantity.asset="SYN";quantity.quantity="2";quantity.kind=AssetComponentKind::Holding;
        quantity.evidence={"Binance 공식 수량","https://api.binance.com/api/v3/account",snapshot.accountScope,0,at,true};snapshot.components={quantity};
        AssetQuote quote{"SYN","KRW","100",{"업비트 공식 시세","https://api.upbit.com/v1/ticker","public:upbit:KRW-SYN",at-5,at,true}};
        PortfolioConnection connection;connection.account.id=snapshot.connectionId;connection.account.provider=snapshot.provider;connection.account.market="spot";connection.account.alias="FAKE_ALIAS";connection.state="complete";
        const auto valuation=AssetValuation::value({snapshot},{quote},"KRW",at,DisplayFreshnessMs);QVERIFY(valuation.complete);
        const auto view=recorded?recordedPortfolio({snapshot},{quote},{connection},at+900000):presentPortfolio({snapshot},valuation,{connection});
        QVERIFY(view.summary.complete);QCOMPARE(view.groups.size(),1);const auto holding=view.groups.first().holdings.first();
        QCOMPARE(holding.value,QString("200"));QCOMPARE(holding.balanceEvidence,quantity.evidence);
        QCOMPARE(holding.balanceEvidence.sourceTimeMs,qint64(0));QCOMPARE(holding.balanceEvidence.receivedAtMs,at);
        QCOMPARE(holding.balanceEvidence.scope,QString("FAKE_PRIVATE_SCOPE"));QCOMPARE(holding.priceEvidence,quote.evidence);
        QCOMPARE(holding.asOfTimeMs,recorded?at:qint64(0));
    }
};
QTEST_MAIN(PortfolioPresentationTest)
#include "tst_portfolio_presentation.moc"
