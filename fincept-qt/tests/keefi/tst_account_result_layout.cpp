#include "keefi/AccountDialog.h"
#include "keefi/AccountModel.h"
#include "keefi/LocalWindow.h"
#include "keefi/PortfolioController.h"
#include "keefi/TradingTransport.h"
#include "keefi/SolanaReadRpc.h"
#include "keefi/Vault.h"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTest>
#include <QTimer>

using namespace keefi;

namespace {
class ResultTransport final : public TradingTransport {
  public:
    bool hold = false;
    void abort() override {}
    void send(quint64 token, const RequestSpec& request) override {
        QVERIFY(!isMutation(request.operation));
        if (hold) return;
        HttpResult result;
        result.status = 200;
        result.body = request.operation == Operation::Time
            ? QJsonDocument(QJsonObject{{"serverTime", QString::number(QDateTime::currentMSecsSinceEpoch())}}).toJson()
            : QByteArray(R"({"canTrade":true,"accountType":"SPOT","balances":[{"asset":"USDT","free":"10","locked":"0"}]})");
        QTimer::singleShot(1, this, [this, token, result] { emit finished(token, result); });
    }
};

class WalletRpc final : public SwapTransport {
  public:
    QString lamports = "0";
    bool fail = false;
    void abort() override {}
    void send(quint64 token, const SwapRequest& request) override {
        const auto rpcId = QJsonDocument::fromJson(request.body).object().value("id");
        const QJsonObject context{{"slot","100"}};
        QJsonValue data;
        if (request.operation == SwapOperation::Genesis) data = "5eykt4UsFv8P8NJdTREpY1vzqKqZKvdpKuc147dw2N9d";
        else if (request.operation == SwapOperation::Balance) data = QJsonObject{{"context",context},{"value",lamports}};
        else {
            QCOMPARE(request.operation, SwapOperation::TokenAccounts);
            data = QJsonObject{{"context",context},{"value",QJsonArray{}}};
        }
        HttpResult result;
        result.status = fail ? 503 : 200;
        result.body = QJsonDocument(QJsonObject{{"jsonrpc","2.0"},{"id",rpcId},{"result",data}}).toJson();
        QTimer::singleShot(1, this, [this, token, result] { emit finished(token,result); });
    }
};

class WalletPerps final : public TradingTransport {
  public:
    QString equity = "0", size = "0";
    bool unified = false, fail = false;
    void abort() override {}
    void send(quint64 token, const RequestSpec& request) override {
        const auto type = QJsonDocument::fromJson(request.body).object().value("type").toString();
        HttpResult result;
        result.status = fail ? 503 : 200;
        if (type == "userAbstraction") result.body = unified ? R"("unifiedAccount")" : R"("disabled")";
        else if (type == "metaAndAssetCtxs")
            result.body = R"([{"collateralToken":"0","universe":[{"name":"ETH"}]},[{}]])";
        else if (type == "spotClearinghouseState")
            result.body = QJsonDocument(QJsonObject{{"balances",QJsonArray{QJsonObject{
                {"coin","USDC"},{"token","0"},{"total",equity},{"hold","0"}}}}}).toJson();
        else {
            QCOMPARE(type, QStringLiteral("clearinghouseState"));
            const QJsonObject summary{{"accountValue",equity},{"totalRawUsd",equity},{"totalMarginUsed","0"},{"totalNtlPos","0"}};
            // An explicit zero-size position row is a closed position.
            const QJsonObject position{{"coin","ETH"},{"szi",size},{"entryPx","1"},{"positionValue","1"},
                {"marginUsed","0"},{"unrealizedPnl","0"},{"liquidationPx",QJsonValue::Null},
                {"leverage",QJsonObject{{"type","cross"},{"value","1"}}}};
            result.body = QJsonDocument(QJsonObject{{"marginSummary",summary},{"crossMarginSummary",summary},
                {"crossMaintenanceMarginUsed","0"},{"withdrawable","0"},{"time",QString::number(QDateTime::currentMSecsSinceEpoch())},
                {"assetPositions",QJsonArray{QJsonObject{{"type","oneWay"},{"position",position}}}}}).toJson();
        }
        QTimer::singleShot(1, this, [this, token, result] { emit finished(token,result); });
    }
};

AccountRecord fakeAccount() {
    AccountRecord account;
    account.provider = "binance";
    account.alias = QStringLiteral("레이아웃 검증 계좌");
    account.environment = "live";
    account.market = "spot";
    account.currency = "USDT";
    return account;
}

PortfolioController::Factories factories(ResultTransport& transport) {
    PortfolioController::Factories result;
    result.trading = [&transport](const AccountRecord&) { return &transport; };
    result.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    return result;
}

void savePreview(QWidget& widget, const QString& name) {
    const auto path = qEnvironmentVariable("KEEFI_UI_PREVIEW_DIR");
    if (!path.isEmpty())
        QVERIFY(widget.grab().save(QDir(path).filePath(name)));
}
}

class AccountResultLayoutTest final : public QObject {
    Q_OBJECT
  private slots:
    void phantomReadReflectsCurrentResponse_data();
    void phantomReadReflectsCurrentResponse();
    void resultCompactsAfterTallCredentialForm();
    void longResultScrollsAndRetryStaysReachable();
};

void AccountResultLayoutTest::phantomReadReflectsCurrentResponse_data() {
    QTest::addColumn<QString>("market");
    QTest::addColumn<QString>("balance");
    QTest::addColumn<QString>("position");
    QTest::addColumn<bool>("partial");
    QTest::addColumn<QString>("detail");
    QTest::addColumn<bool>("hasAssets");
    const QString sol = "solana", hl = "hyperliquid_perps";
    QTest::newRow("zero-SOL") << sol << QString("0") << QString("0") << false << QString("보유 자산 없음") << false;
    QTest::newRow("one-lamport") << sol << QString("1") << QString("0") << false << QString("보유 자산 1종") << true;
    QTest::newRow("flat-zero-equity") << hl << QString("0.00") << QString("-0.0") << false << QString("잔고와 열린 포지션 없음") << false;
    QTest::newRow("collateral-only") << hl << QString("10") << QString("0") << false << QString("열린 포지션 0개") << true;
    QTest::newRow("short-zero-equity") << hl << QString("0") << QString("-0.1") << false << QString("열린 포지션 1개") << true;
    QTest::newRow("unified-partial") << hl << QString("0") << QString("0") << true << QString("전체 순자산은 미확정") << false;
}

void AccountResultLayoutTest::phantomReadReflectsCurrentResponse() {
    QFETCH(QString,market); QFETCH(QString,balance); QFETCH(QString,position);
    QFETCH(bool,partial); QFETCH(QString,detail); QFETCH(bool,hasAssets);
    QTemporaryDir dir;
    Vault vault(dir.filePath("test-wallet"));
    QVERIFY(vault.create("Test Wallet Results! 2026"));
    const bool solana = market == "solana";
    AccountRecord account;
    account.provider = "phantom"; account.market = market; account.environment = "mainnet";
    account.currency = solana ? "SOL" : "USDC"; account.alias = "Test wallet";
    const auto address = solana ? "11111111111111111111111111111111" : "0x1111111111111111111111111111111111111111";
    QVERIFY(vault.addAccount(account,{{"address",address}},StorageMode::EncryptedVault));
    const auto id = vault.accounts().first().id;
    WalletRpc rpc; rpc.lamports = balance;
    WalletPerps perps; perps.equity = balance; perps.size = position; perps.unified = partial;
    PortfolioController::Factories f;
    f.wallet = [&](const AccountRecord&) { return &rpc; };
    f.trading = [&](const AccountRecord&) { return &perps; };
    f.prices = [] { return static_cast<TradingTransport*>(nullptr); };
    PortfolioController portfolio(vault,nullptr,f);
    AccountModel accounts(&vault);
    LocalWindow window(vault, accounts);
    window.setPortfolioController(&portfolio);
    window.setSecurityStatus(true);
    auto* tabs = window.findChild<QTabWidget*>("portfolioPages");
    auto* page = window.findChild<QWidget*>("connectionsPage");
    QVERIFY(tabs && page);
    tabs->setCurrentWidget(page);
    auto* state = page->findChild<QLabel*>("connectionReadState");
    auto* scope = page->findChild<QLabel*>("connectionActionScope");
    auto* assets = page->findChild<QPushButton*>("connectionAssets");
    QVERIFY(state && scope && assets);
    AccountDialog dialog(true, &window);
    dialog.show();
    portfolio.setAutomaticRefresh(true);
    dialog.showConnectionResult(id,&portfolio);
    auto* label = dialog.findChild<QLabel*>("accountConnectionResult");
    auto* finish = dialog.findChild<QPushButton*>("accountConnectionFinish");
    auto* retry = dialog.findChild<QPushButton*>("accountConnectionRetry");
    QVERIFY(label && finish && retry);
    const auto visibleText = [&] {
        QTextDocument doc;
        doc.setHtml(label->text());
        return doc.toPlainText();
    };
    const auto network = solana ? QStringLiteral("Solana") : QStringLiteral("Hyperliquid");
    QVERIFY(visibleText().contains(network + " 조회 중"));
    const auto done = network + (partial ? " 부분 조회" : " 조회 완료");
    QTRY_VERIFY_WITH_TIMEOUT(visibleText().contains(done),5000);
    QVERIFY2(visibleText().contains(detail), qPrintable(visibleText()));
    QTRY_COMPARE_WITH_TIMEOUT(state->text(), done, 5000);
    QVERIFY2(scope->text().contains(detail), qPrintable(scope->text()));
    QCOMPARE(assets->isEnabled(), hasAssets);
    QCOMPARE(finish->text(), hasAssets ? QStringLiteral("보유 자산 보기") : QStringLiteral("연결 상태 보기"));
    QVERIFY(portfolio.completedWalletRead(id));
    QVERIFY(!portfolio.completedWalletRead("different-connection"));

    rpc.fail = perps.fail = true;
    portfolio.refreshConnection(id);
    QTRY_VERIFY_WITH_TIMEOUT(visibleText().contains(network + " 조회 실패"),5000);
    QVERIFY(!portfolio.completedWalletRead(id));
    QVERIFY(!visibleText().contains(detail));
    QTRY_COMPARE_WITH_TIMEOUT(state->text(), network + " 조회 실패", 5000);
    QVERIFY(!scope->text().contains(detail));
    QVERIFY(!assets->isEnabled());
    QVERIFY(retry->isVisible());
    QCOMPARE(finish->text(), QStringLiteral("연결 상태 보기"));
    rpc.fail = perps.fail = false;
    retry->click();
    QTRY_VERIFY_WITH_TIMEOUT(visibleText().contains(done),5000);
    QVERIFY(visibleText().contains(detail));
    QTRY_COMPARE_WITH_TIMEOUT(state->text(), done, 5000);
    QVERIFY(scope->text().contains(detail));
    QCOMPARE(assets->isEnabled(), hasAssets);
    vault.lock();
    QVERIFY(!portfolio.completedWalletRead(id));
    QVERIFY(!visibleText().contains(detail));
}

void AccountResultLayoutTest::resultCompactsAfterTallCredentialForm() {
    QTemporaryDir directory;
    Vault vault(directory.filePath("fake-layout-vault"));
    QVERIFY(vault.create("Fake Layout Only! 2026"));
    QVERIFY(vault.addAccount(fakeAccount(), {{"api_key","FakeLayoutKey123"},{"secret","FakeLayoutSecret123"}},
        StorageMode::EncryptedVault));
    ResultTransport transport;
    PortfolioController portfolio(vault, nullptr, factories(transport));
    AccountModel accounts(&vault);
    LocalWindow host(vault, accounts);
    AccountDialog dialog(true, &host);
    auto* provider = dialog.findChild<QComboBox*>("providerSelector");
    provider->setCurrentIndex(provider->findData("okx")); // The hidden, taller form must not size the result.
    auto* secret = dialog.findChild<QLineEdit*>("credential_secret");
    QVERIFY(secret);
    secret->setText("FAKE-LAYOUT-SECRET");
    dialog.resize(600, 680);
    dialog.show();
    QCoreApplication::processEvents();
    portfolio.setAutomaticRefresh(true);
    dialog.showConnectionResult(vault.accounts().first().id, &portfolio);
    auto* result = dialog.findChild<QLabel*>("accountConnectionResult");
    auto* finish = dialog.findChild<QPushButton*>("accountConnectionFinish");
    QVERIFY(result);
    QVERIFY(finish);
    QTRY_VERIFY_WITH_TIMEOUT(result->text().contains(QStringLiteral("잔고 조회 완료")), 3000);
    QCoreApplication::processEvents();
    savePreview(dialog, "connection-result-success.png");
    qInfo("result geometry: %d x %d, minimum hint: %d x %d",
        dialog.width(), dialog.height(), dialog.minimumSizeHint().width(), dialog.minimumSizeHint().height());
    QVERIFY2(dialog.height() <= 280, qPrintable(QString("Result retains %1 px of form height").arg(dialog.height())));
    QVERIFY(dialog.width() >= 540 && dialog.width() <= 640);
    QVERIFY(dialog.minimumSizeHint().height() <= 280);
    QVERIFY(secret->text().isEmpty());
    // The result still confirms the save; how keys are stored moved to the user guide.
    QVERIFY(result->text().contains(QStringLiteral("저장 완료")));
    QVERIFY(!result->text().contains(QStringLiteral("암호화")));
    QVERIFY(!result->text().contains(QStringLiteral("원화 평가")));
    QCOMPARE(finish->text(), QStringLiteral("보유 자산 보기"));
    QVERIFY(dialog.rect().contains(QRect(finish->mapTo(&dialog, QPoint()), finish->size())));
    const auto textBottom = result->mapTo(&dialog, QPoint(0, result->heightForWidth(result->width()))).y();
    QVERIFY(finish->mapTo(&dialog, QPoint()).y() - textBottom < 80);
}

void AccountResultLayoutTest::longResultScrollsAndRetryStaysReachable() {
    QTemporaryDir directory;
    Vault vault(directory.filePath("fake-layout-vault"));
    QVERIFY(vault.create("Fake Layout Only! 2026"));
    QVERIFY(vault.addAccount(fakeAccount(), {{"api_key","FakeRetryKey123"},{"secret","FakeRetrySecret123"}},
        StorageMode::EncryptedVault));
    ResultTransport transport;
    transport.hold = true;
    PortfolioController portfolio(vault, nullptr, factories(transport));
    AccountModel accounts(&vault);
    LocalWindow host(vault, accounts);
    AccountDialog dialog(true, &host);
    portfolio.setAutomaticRefresh(true);
    dialog.showConnectionResult(vault.accounts().first().id, &portfolio);
    dialog.show();
    auto* result = dialog.findChild<QLabel*>("accountConnectionResult");
    auto* retry = dialog.findChild<QPushButton*>("accountConnectionRetry");
    auto* finish = dialog.findChild<QPushButton*>("accountConnectionFinish");
    QVERIFY(result);
    QVERIFY(retry);
    QVERIFY(finish);
    QTRY_VERIFY(result->text().contains(QStringLiteral("조회 중")));
    portfolio.stopReads();
    QTRY_VERIFY(retry->isVisible());
    QCOMPARE(finish->text(), QStringLiteral("연결 상태 보기"));
    savePreview(dialog, "connection-result-retry.png");

    // Exercise the presentation boundary with a long localized explanation,
    // never by passing an unredacted provider response to the dialog.
    const QString explanation = QStringLiteral("조회 중단\n")
        + QStringLiteral("연결을 확인한 후 다시 조회하세요. 이 문장은 좁은 화면과 긴 안내의 줄바꿈 검증용입니다.\n").repeated(18)
        + QStringLiteral("암호화 저장 완료");
    QSignalBlocker pauseStatusRefresh(&portfolio);
    result->setText(explanation);
    dialog.resize(560, 260);
    QCoreApplication::processEvents();
    auto* scroll = dialog.findChild<QScrollArea*>("accountResultScroll");
    QVERIFY2(scroll, "Long result needs its own scroll area, separate from the hidden input form.");
    QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
    QCOMPARE(result->text(), explanation);
    QVERIFY(result->height() >= result->heightForWidth(result->width()));
    QVERIFY(dialog.height() <= 280);
    QVERIFY(dialog.rect().contains(QRect(finish->mapTo(&dialog, QPoint()), finish->size())));
    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    QCoreApplication::processEvents();
    QVERIFY(scroll->viewport()->rect().contains(
        QRect(retry->mapTo(scroll->viewport(), QPoint()), retry->size())));
    savePreview(dialog, "connection-result-long-scroll.png");

    pauseStatusRefresh.unblock();
    transport.hold = false;
    retry->click();
    QTRY_VERIFY_WITH_TIMEOUT(result->text().contains(QStringLiteral("잔고 조회 완료")), 3000);
    QVERIFY(!retry->isVisible());
    QVERIFY(dialog.height() <= 280);
    QCOMPARE(finish->text(), QStringLiteral("보유 자산 보기"));
}

QTEST_MAIN(AccountResultLayoutTest)
#include "tst_account_result_layout.moc"
