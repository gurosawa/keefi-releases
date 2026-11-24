#include "keefi/AccountDialog.h"
#include "keefi/KeefiTheme.h"
#include "keefi/AccountModel.h"

#include <QComboBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTest>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

using namespace keefi;

namespace {
template<class T> T* child(QWidget& widget, const char* name) {
    auto* result = widget.findChild<T*>(QString::fromLatin1(name));
    if (!result) qFatal("Missing form control: %s", name);
    return result;
}

bool chooseService(AccountDialog& dialog, const QString& provider) {
    auto* tile = dialog.findChild<QAbstractButton*>(QStringLiteral("accountService_") + provider);
    if (!tile) return false;
    QTest::mouseClick(tile, Qt::LeftButton);
    return child<QStackedWidget>(dialog, "accountConnectionSteps")->currentIndex() == 1;
}

void fillFakeCredentials(AccountDialog& dialog, const QString& provider) {
    const auto* specification = AccountModel::provider(provider);
    if (!specification) qFatal("Unknown form provider");
    for (const auto& field : specification->fields)
        child<QLineEdit>(dialog, qPrintable("credential_" + field.key))->setText(
            field.key == "address" ? QString(32, '1') : QStringLiteral("Fake_Form_Only_2026"));
}
}

class ConnectionFormTest final : public QObject {
    Q_OBJECT
  public slots:
    void captureOfficialPage(const QUrl& url) { openedUrl_ = url; }
  private:
    QUrl openedUrl_;
  private slots:
    void initTestCase() { qRegisterMetaType<AccountDraft>(); }
    void cleanup() { KeefiTheme::instance()->setMode(ThemeMode::System); }
    void themeChangePreservesDraftAndRecolorsOfficialGuide();
    void singleEnvironmentIsHiddenButSubmitted_data();
    void singleEnvironmentIsHiddenButSubmitted();
    void suspendedBrokersRejectNewAndExistingCredentials();
    void stockChartChooserUsesLiveBrokersAndReturnsToChart();
    void registrationLinksOpenTheSelectedProvider_data();
    void registrationLinksOpenTheSelectedProvider();
    void kiwoomFilePairIsValidatedBeforeUpdatingEitherField();
    void newOkxUsesGlobalWithoutARegionChoice();
    void replacingOkxPreservesTheRegisteredRegion_data();
    void replacingOkxPreservesTheRegisteredRegion();
    void okxFuturesStaysGlobal();
    void phantomGuideOpensWithoutSendingTheAddress_data();
    void phantomGuideOpensWithoutSendingTheAddress();
    void phantomMarketsUseSeparateAddressForms();
    void phantomSolanaAddressChecksDecodedLength_data();
    void phantomSolanaAddressChecksDecodedLength();
    void phantomPastedAddressCanSwitchMarkets();
    void phantomReplacementKeepsTheSelectedMarket();
    void phantomPerpsAddressIsRequiredAndValidated_data();
    void phantomPerpsAddressIsRequiredAndValidated();
};

void ConnectionFormTest::themeChangePreservesDraftAndRecolorsOfficialGuide() {
    KeefiTheme::instance()->setMode(ThemeMode::Light);
    AccountDialog dialog(true);
    dialog.show();
    QCoreApplication::processEvents();
    QVERIFY(chooseService(dialog, "okx"));
    child<QLineEdit>(dialog, "accountAlias")->setText(QStringLiteral("합성 테마 전환"));
    fillFakeCredentials(dialog, "okx");
    auto* passphrase = child<QLineEdit>(dialog, "credential_passphrase");
    passphrase->setText(" Fake theme passphrase ");
    auto* guide = child<QLabel>(dialog, "providerGuide");
    QVERIFY(guide->text().contains(themeColors().accentText.name()));
    const auto originalGuide = guide->text();
    QSignalSpy submitted(&dialog, &AccountDialog::submitted);
    KeefiTheme::instance()->setMode(ThemeMode::Dark);
    QCoreApplication::processEvents();
    QVERIFY(guide->text() != originalGuide);
    QVERIFY(guide->text().contains(themeColors().accentText.name()));
    QCOMPARE(passphrase->text(), QString(" Fake theme passphrase "));
    QCOMPARE(passphrase->echoMode(), QLineEdit::Password);
    QCOMPARE(child<QLineEdit>(dialog, "accountAlias")->text(), QStringLiteral("합성 테마 전환"));
    QCOMPARE(child<QComboBox>(dialog, "providerSelector")->currentData().toString(), QString("okx"));
    QCOMPARE(child<QStackedWidget>(dialog, "accountConnectionSteps")->currentIndex(), 1);
    QVERIFY(child<QPushButton>(dialog, "saveAccount")->isEnabled());
    QCOMPARE(submitted.size(), 0);
}

void ConnectionFormTest::singleEnvironmentIsHiddenButSubmitted_data() {
    QTest::addColumn<QString>("provider");
    QTest::addColumn<QString>("environment");
    for (const auto* provider : {"binance", "upbit", "bithumb", "toss", "kiwoom"})
        QTest::newRow(provider) << QString::fromLatin1(provider) << QString("live");
    QTest::newRow("phantom") << QString("phantom") << QString("mainnet");
}

void ConnectionFormTest::suspendedBrokersRejectNewAndExistingCredentials() {
    for (const auto& id : {QStringLiteral("kis"), QStringLiteral("ls")}) {
        AccountDialog dialog(true);
        auto* tile = dialog.findChild<QAbstractButton*>("accountService_" + id);
        QVERIFY(tile && !tile->isEnabled());
        QVERIFY(!AccountModel::allowsNewConnection(id));
        tile->click();
        auto* steps = child<QStackedWidget>(dialog, "accountConnectionSteps");
        QCOMPARE(steps->currentIndex(), 0);
        auto* selector = child<QComboBox>(dialog, "providerSelector");
        QCOMPARE(selector->findData(id), -1);
        AccountDraft legacy;
        legacy.provider = id; legacy.alias = "기존 연결"; legacy.market = "domestic_equity";
        legacy.currency = "KRW"; legacy.environment = "live";
        QSignalSpy submitted(&dialog, &AccountDialog::submitted);
        QSignalSpy rejected(&dialog, &QDialog::rejected);
        dialog.setReplacement(legacy);
        QCOMPARE(rejected.size(), 1);
        QVERIFY(!dialog.findChild<QLineEdit*>("credential_app_secret"));
        auto* save = child<QPushButton>(dialog, "saveAccount");
        QVERIFY(!save->isEnabled());
        save->click();
        QCOMPARE(submitted.size(), 0);
    }
}

void ConnectionFormTest::stockChartChooserUsesLiveBrokersAndReturnsToChart() {
    AccountDialog dialog(true, nullptr, true);
    dialog.show();
    QCOMPARE(dialog.windowTitle(), QStringLiteral("증권 연결하기"));
    for (const auto* id : {"toss", "kiwoom"})
        QVERIFY(dialog.findChild<QAbstractButton*>(QStringLiteral("accountService_") + id)->isEnabled());
    for (const auto* id : {"kis", "ls", "binance", "phantom"})
        QVERIFY(!dialog.findChild<QAbstractButton*>(QStringLiteral("accountService_") + id));
    QList<AccountRecord> accounts;
    for (const auto* id : {"toss", "kiwoom", "ls", "binance"}) {
        AccountRecord record;
        record.provider = id; record.environment = "live"; record.market = "domestic_equity";
        record.id = id; record.alias = "합성&계좌"; accounts.append(record);
    }
    auto paper = accounts[1]; paper.environment = "paper"; paper.id = "paper"; accounts.append(paper);
    dialog.setCredentialSources(accounts);
    dialog.setCredentialSources(accounts);
    QVERIFY(!dialog.findChild<QPushButton*>("existingChartConnection_ls"));
    QVERIFY(!dialog.findChild<QPushButton*>("existingChartConnection_binance"));
    QVERIFY(!dialog.findChild<QPushButton*>("existingChartConnection_paper"));
    QSignalSpy reused(&dialog, &AccountDialog::existingChartConnectionRequested);
    auto* existing = child<QPushButton>(dialog, "existingChartConnection_kiwoom");
    QVERIFY(existing->accessibleName().contains("합성&계좌"));
    existing->click();
    QCOMPARE(reused.size(), 1);
    QCOMPARE(reused.first().first().toString(), QString("kiwoom"));
    QVERIFY(chooseService(dialog, "toss"));
    child<QPushButton>(dialog, "accountServiceBack")->click();
    QVERIFY(chooseService(dialog, "kiwoom"));
    QSignalSpy returned(&dialog, &AccountDialog::chartReturnRequested);
    QSignalSpy assets(&dialog, &AccountDialog::assetsRequested);
    dialog.showConnectionResult("kiwoom", nullptr);
    QCoreApplication::processEvents();
    QCOMPARE(returned.size(), 0);
    auto* finish = child<QPushButton>(dialog, "accountConnectionFinish");
    QCOMPARE(finish->text(), QStringLiteral("보던 차트로 돌아가기"));
    finish->click();
    finish->click();
    QCOMPARE(returned.size(), 1);
    QCOMPARE(returned.first().first().toString(), QString("kiwoom"));
    QCOMPARE(assets.size(), 0);
}

void ConnectionFormTest::registrationLinksOpenTheSelectedProvider_data() {
    QTest::addColumn<QString>("provider");
    QTest::addColumn<QString>("issuer");
    QTest::newRow("toss") << QString("toss") << QString("https://www.tossinvest.com/open-api/landing");
    QTest::newRow("kiwoom") << QString("kiwoom") << QString("https://openapi.kiwoom.com/mgmt/VOpenApiRegView");
    QTest::newRow("binance") << QString("binance") << QString("https://www.binance.com/en/my/settings/api-management");
    QTest::newRow("upbit") << QString("upbit") << QString("https://upbit.com/mypage/open_api_management");
    QTest::newRow("bithumb") << QString("bithumb") << QString("https://www.bithumb.com/react/api-support/management-api");
    QTest::newRow("okx") << QString("okx") << QString("https://www.okx.com/account/my-api");
}

void ConnectionFormTest::registrationLinksOpenTheSelectedProvider() {
    QFETCH(QString, provider);
    QFETCH(QString, issuer);
    AccountDialog dialog(true);
    dialog.show();
    QVERIFY(chooseService(dialog, provider));
    auto* guide = child<QLabel>(dialog, "providerGuide");
    const auto* spec = AccountModel::provider(provider);
    QVERIFY(guide->openExternalLinks());
    QVERIFY(guide->text().contains(spec->guideUrl));
    const QUrl url(spec->guideUrl);
    QCOMPARE(url.scheme(), QString("https"));
    QVERIFY(url.host().endsWith(".azurestaticapps.net"));
    QCOMPARE(url.path(), QString("/guides/%1/").arg(provider));
    auto* issue = child<QPushButton>(dialog, "providerIssueKey");
    QVERIFY(issue->isVisible());
    openedUrl_.clear();
    QDesktopServices::setUrlHandler("https", this, "captureOfficialPage");
    issue->click();
    QDesktopServices::unsetUrlHandler("https");
    QCOMPARE(openedUrl_, QUrl(issuer));
}

void ConnectionFormTest::kiwoomFilePairIsValidatedBeforeUpdatingEitherField() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto write = [&dir](const QString& name, const QByteArray& data) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) return QString{};
        return file.fileName();
    };
    AccountDialog dialog(true);
    dialog.show();
    QVERIFY(chooseService(dialog, "kiwoom"));
    auto* import = child<QPushButton>(dialog, "importKiwoomKeyFiles");
    QVERIFY(import->isVisible());
    QVERIFY(!import->autoDefault());
    auto* key = child<QLineEdit>(dialog, "credential_app_key");
    auto* secret = child<QLineEdit>(dialog, "credential_app_secret");
    QSignalSpy submitted(&dialog, &AccountDialog::submitted);
    const auto keyFile = write("0000000001_appkey (1).txt", QByteArray::fromHex("efbbbf") + "FAKE-KEY==\r\n");
    const auto secretFile = write("0000000001_appsecret.txt", "FAKE-SECRET/+==\n");
    // Exercise the actual import action with Qt's automated, local file chooser.
    const bool hadNonNativeDialogs = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    bool selected = false;
    QTimer::singleShot(0, &dialog, [&] {
        auto* picker = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        if (!picker) return;
        auto* names = picker->findChild<QLineEdit*>("fileNameEdit");
        if (!names || picker->fileMode() != QFileDialog::ExistingFiles) { picker->reject(); return; }
        picker->setDirectory(dir.path());
        names->setText(QStringLiteral("\"%1\" \"%2\"").arg(QFileInfo(secretFile).fileName(), QFileInfo(keyFile).fileName()));
        selected = QMetaObject::invokeMethod(picker, "accept");
        if (picker->isVisible()) picker->reject();
    });
    import->click();
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, hadNonNativeDialogs);
    QVERIFY(selected);
    QCOMPARE(key->text(), QString("FAKE-KEY=="));
    QCOMPARE(secret->text(), QString("FAKE-SECRET/+=="));
    QCOMPARE(key->echoMode(), QLineEdit::Password);
    QCOMPARE(secret->echoMode(), QLineEdit::Password);
    QVERIFY(child<QLineEdit>(dialog, "credential_account_number")->text().isEmpty());
    QVERIFY(child<QPushButton>(dialog, "saveAccount")->isEnabled());
    QCOMPARE(submitted.size(), 0);
    auto* error = child<QLabel>(dialog, "accountError");
    for (const auto& paths : QList<QStringList>{{keyFile}, {keyFile, keyFile}, {keyFile, secretFile, secretFile},
         {keyFile, write("0000000002_appsecret.txt", "OTHER-FAKE")},
         {keyFile, write("random.txt", "FAKE-FILE")},
         {keyFile, dir.filePath("0000000001_appsecret (9).txt")}}) {
        QVERIFY(!dialog.importKiwoomKeyFiles(paths));
        QVERIFY(!error->text().isEmpty());
        QVERIFY(!error->text().contains("0000000001"));
    }
    for (const auto& invalid : {QByteArray{}, QByteArray("line1\nline2"), QByteArray("a\0b", 3), QByteArray(8250, 'x')}) {
        QVERIFY(!dialog.importKiwoomKeyFiles({keyFile, write("0000000001_appsecret.txt", invalid)}));
        QCOMPARE(key->text(), QString("FAKE-KEY=="));
        QCOMPARE(secret->text(), QString("FAKE-SECRET/+=="));
        QVERIFY(!error->text().contains("FAKE"));
    }
    QCOMPARE(submitted.size(), 0);
    child<QPushButton>(dialog, "accountServiceBack")->click();
    QVERIFY(key->text().isEmpty() && secret->text().isEmpty());
    QVERIFY(chooseService(dialog, "toss"));
    QVERIFY(!dialog.findChild<QPushButton*>("importKiwoomKeyFiles"));
    QVERIFY(!dialog.importKiwoomKeyFiles({keyFile, secretFile}));
}

void ConnectionFormTest::singleEnvironmentIsHiddenButSubmitted() {
    QFETCH(QString, provider);
    QFETCH(QString, environment);
    AccountDialog dialog(true);
    dialog.show();
    QCoreApplication::processEvents();
    QVERIFY(chooseService(dialog, provider));
    QVERIFY(!child<QComboBox>(dialog, "environmentSelector")->isVisible());
    for (auto* label : dialog.findChildren<QLabel*>())
        if (label->text() == QStringLiteral("환경")) QVERIFY(!label->isVisible());

    child<QLineEdit>(dialog, "accountAlias")->setText(QStringLiteral("가짜 폼 검증"));
    fillFakeCredentials(dialog, provider);
    QSignalSpy submitted(&dialog, &AccountDialog::submitted);
    auto* save = child<QPushButton>(dialog, "saveAccount");
    QVERIFY(save->isEnabled());
    QTest::mouseClick(save, Qt::LeftButton);
    QCOMPARE(submitted.size(), 1);
    const auto draft = submitted.first().first().value<AccountDraft>();
    QCOMPARE(draft.provider, provider);
    QCOMPARE(draft.environment, environment);
}

void ConnectionFormTest::newOkxUsesGlobalWithoutARegionChoice() {
    AccountDialog dialog(true);
    dialog.show();
    QCoreApplication::processEvents();
    QVERIFY(chooseService(dialog, "okx"));
    auto* region = child<QComboBox>(dialog, "okxRegionSelector");
    QCOMPARE(region->currentData().toString(), QString("global"));
    QVERIFY(!region->isVisible());
    QVERIFY(child<QLabel>(dialog, "accountStageStatus")->text().contains("Global"));
    for (auto* label : dialog.findChildren<QLabel*>())
        if (label->text() == QStringLiteral("OKX 계정 등록 지역")) QVERIFY(!label->isVisible());
    child<QLineEdit>(dialog, "accountAlias")->setText(QStringLiteral("가짜 Global 폼"));
    fillFakeCredentials(dialog, "okx");
    child<QLineEdit>(dialog, "credential_passphrase")->setText(" Fake passphrase ");
    QSignalSpy submitted(&dialog, &AccountDialog::submitted);
    auto* save = child<QPushButton>(dialog, "saveAccount");
    QVERIFY(save->isEnabled());
    QTest::mouseClick(save, Qt::LeftButton);
    QCOMPARE(submitted.size(), 1);
    const auto draft = submitted.first().first().value<AccountDraft>();
    QCOMPARE(draft.region, QString("global"));
    QCOMPARE(draft.environment, QString("live"));
    QCOMPARE(draft.credentials.value("passphrase"), QString(" Fake passphrase "));
}

void ConnectionFormTest::replacingOkxPreservesTheRegisteredRegion_data() {
    QTest::addColumn<QString>("region");
    QTest::addColumn<QString>("shownRegion");
    QTest::newRow("global") << QString("global") << QString("Global");
    QTest::newRow("us") << QString("us") << QStringLiteral("미국·호주");
    QTest::newRow("eea") << QString("eea") << QString("EEA");
    QTest::newRow("unrecorded") << QString{} << QStringLiteral("미확인");
}

void ConnectionFormTest::replacingOkxPreservesTheRegisteredRegion() {
    QFETCH(QString, region);
    QFETCH(QString, shownRegion);
    AccountDraft original;
    original.provider = "okx"; original.alias = QStringLiteral("가짜 기존 OKX");
    original.environment = "live"; original.market = "spot";
    original.currency = "USDT"; original.region = region;
    AccountDialog dialog(true);
    dialog.setReplacement(original);
    dialog.show();
    QCoreApplication::processEvents();
    auto* selector = child<QComboBox>(dialog, "okxRegionSelector");
    QVERIFY(!selector->isVisible());
    QVERIFY(!selector->isEnabled());
    QCOMPARE(selector->currentData().toString(), region);
    QVERIFY(child<QLabel>(dialog, "accountStageStatus")->text().contains(shownRegion));
    fillFakeCredentials(dialog, "okx");
    QSignalSpy submitted(&dialog, &AccountDialog::submitted);
    auto* save = child<QPushButton>(dialog, "saveAccount");
    QCOMPARE(save->isEnabled(), !region.isEmpty());
    QTest::mouseClick(save, Qt::LeftButton);
    QCOMPARE(submitted.size(), region.isEmpty() ? 0 : 1);
    if (!region.isEmpty()) {
        const auto draft = submitted.first().first().value<AccountDraft>();
        QCOMPARE(draft.region, region);
        QCOMPARE(draft.market, QString("spot"));
    }
}

void ConnectionFormTest::okxFuturesStaysGlobal() {
    AccountDialog dialog(true);
    dialog.show();
    QCoreApplication::processEvents();
    QVERIFY(chooseService(dialog, "okx"));
    auto* market = child<QComboBox>(dialog, "marketSelector");
    const int futures = market->findData("okx_futures");
    QVERIFY(futures >= 0);
    QCOMPARE(market->itemText(futures), QStringLiteral("SWAP·FUTURES"));
    market->setCurrentIndex(futures);
    QVERIFY(market->isVisible());
    QVERIFY(market->isEnabled());
    QVERIFY(!child<QComboBox>(dialog, "okxRegionSelector")->isVisible());
    child<QLineEdit>(dialog, "accountAlias")->setText(QStringLiteral("가짜 OKX 파생 연결"));
    fillFakeCredentials(dialog, "okx");
    QSignalSpy submitted(&dialog, &AccountDialog::submitted);
    auto* save = child<QPushButton>(dialog, "saveAccount");
    QVERIFY(save->isEnabled());
    QTest::mouseClick(save, Qt::LeftButton);
    QCOMPARE(submitted.size(), 1);
    const auto draft = submitted.first().first().value<AccountDraft>();
    QCOMPARE(draft.market, QString("okx_futures"));
    QCOMPARE(draft.region, QString("global"));
}

void ConnectionFormTest::phantomGuideOpensWithoutSendingTheAddress_data() {
    QTest::addColumn<QString>("market");
    QTest::addColumn<QString>("address");
    QTest::newRow("solana") << QString("solana") << QString("11111111111111111111111111111111");
    QTest::newRow("hyperliquid") << QString("hyperliquid_perps") << QString("0x1111111111111111111111111111111111111111");
}

void ConnectionFormTest::phantomGuideOpensWithoutSendingTheAddress() {
    QFETCH(QString, market);
    QFETCH(QString, address);
    AccountDialog dialog(false);
    dialog.show();
    QVERIFY(chooseService(dialog, "phantom"));
    auto* markets = child<QComboBox>(dialog, "marketSelector");
    markets->setCurrentIndex(markets->findData(market));
    auto* addressField = child<QLineEdit>(dialog, "credential_address");
    addressField->setText(address);
    auto* guide = child<QLabel>(dialog, "providerGuide");
    QVERIFY(guide->isVisible());
    QVERIFY(guide->text().contains(QStringLiteral("Phantom 주소 복사 가이드")));
    QVERIFY(!child<QPushButton>(dialog, "providerIssueKey")->isVisible());
    QVERIFY(!child<QPushButton>(dialog, "saveAccount")->isEnabled());

    openedUrl_.clear();
    QSignalSpy submitted(&dialog, &AccountDialog::submitted);
    QDesktopServices::setUrlHandler("https", this, "captureOfficialPage");
    QTest::mouseClick(guide, Qt::LeftButton, {}, QPoint(6, guide->contentsRect().center().y()));
    QDesktopServices::unsetUrlHandler("https");
    const auto url = QUrl(AccountModel::provider("phantom")->guideUrl);
    QCOMPARE(openedUrl_, url);
    QCOMPARE(url.scheme(), QString("https"));
    QCOMPARE(url.host(), QUrl(AccountModel::provider("upbit")->guideUrl).host());
    QCOMPARE(url.path(), QString("/guides/phantom/"));
    QVERIFY(!url.hasQuery() && !url.hasFragment());
    QCOMPARE(addressField->text(), address);
    QCOMPARE(submitted.count(), 0);
}

void ConnectionFormTest::phantomMarketsUseSeparateAddressForms() {
    AccountDialog dialog(true);
    dialog.show();
    QCoreApplication::processEvents();
    QVERIFY(chooseService(dialog, "phantom"));
    auto* market = child<QComboBox>(dialog, "marketSelector");
    const int spot = market->findData("solana"), perps = market->findData("hyperliquid_perps");
    QVERIFY(spot >= 0 && perps >= 0);
    QCOMPARE(market->itemText(spot), QStringLiteral("Solana 지갑(현물)"));
    QCOMPARE(market->itemText(perps), QStringLiteral("무기한 선물(Hyperliquid)"));
    auto* address = child<QLineEdit>(dialog, "credential_address");
    QVERIFY(address->placeholderText().contains("Solana"));
    address->setText("11111111111111111111111111111111");
    market->setCurrentIndex(perps);
    QVERIFY(address->text().isEmpty());
    QVERIFY(address->placeholderText().contains("0x"));
    bool ethereumLabel = false;
    for (const auto* label : dialog.findChildren<QLabel*>())
        ethereumLabel |= label->isVisible() && label->text() == QStringLiteral("조회용 Ethereum 주소");
    QVERIFY(ethereumLabel);
    const auto hint = child<QLabel>(dialog, "accountStageStatus")->text();
    QVERIFY(hint.contains("Hyperliquid") && hint.contains("Ethereum"));
    QVERIFY(hint.contains(QStringLiteral("개인키")) && hint.contains(QStringLiteral("시드")));
    auto* currency = child<QComboBox>(dialog, "currencySelector");
    QCOMPARE(currency->count(), 1);
    QCOMPARE(currency->currentData().toString(), QString("USDC"));
    QVERIFY(!currency->isEnabled());
    QVERIFY(!dialog.findChild<QLineEdit*>("credential_private_key"));
    QVERIFY(!dialog.findChild<QLineEdit*>("credential_seed"));
    address->setText("0x1111111111111111111111111111111111111111");
    market->setCurrentIndex(spot);
    QVERIFY(address->text().isEmpty());
    QVERIFY(address->placeholderText().contains("Solana"));
    QCOMPARE(currency->count(), 2);
    QVERIFY(currency->findData("SOL") >= 0 && currency->findData("USDC") >= 0);
    QVERIFY(currency->isEnabled());
}

void ConnectionFormTest::phantomSolanaAddressChecksDecodedLength_data() {
    QTest::addColumn<QString>("address");
    QTest::addColumn<bool>("valid");
    QTest::newRow("system-address") << QString(32, '1') << true;
    QTest::newRow("leading-zeroes-nonzero") << (QString(31, '1') + "2") << true;
    QTest::newRow("44-base58-characters") << QStringLiteral("9ahYBF9J6simiVmTPwRgxB1t1AiZY5C8xpeLQGh5tJxn") << true;
    QTest::newRow("31-decoded-bytes") << QString(31, '1') << false;
    QTest::newRow("33-decoded-bytes") << QString(33, '1') << false;
    QTest::newRow("zero-not-base58") << (QString(31, '1') + "0") << false;
    QTest::newRow("not-an-address") << QStringLiteral("not an address") << false;
    QTest::newRow("empty") << QString{} << false;
}

void ConnectionFormTest::phantomSolanaAddressChecksDecodedLength() {
    QFETCH(QString, address);
    QFETCH(bool, valid);
    AccountDialog dialog(true);
    dialog.show();
    QVERIFY(chooseService(dialog, "phantom"));
    auto* field = child<QLineEdit>(dialog, "credential_address");
    auto* save = child<QPushButton>(dialog, "saveAccount");
    auto* feedback = child<QLabel>(dialog, "phantomAddressFeedback");
    field->setText(address);
    QCOMPARE(save->isEnabled(), valid);
    QCOMPARE(feedback->isVisible(), !address.isEmpty());
    QVERIFY(!child<QPushButton>(dialog, "switchPhantomAddressMarket")->isVisible());
    if (!address.isEmpty()) {
        QVERIFY(feedback->text().contains("Solana"));
        QCOMPARE(feedback->text().contains(QStringLiteral("형식이 맞아요")), valid);
        QVERIFY(!feedback->text().contains(address));
    }
}

void ConnectionFormTest::phantomPastedAddressCanSwitchMarkets() {
    const QString eth = "0x1111111111111111111111111111111111111111";
    const QString sol(32, '1');
    AccountDialog dialog(true);
    dialog.show();
    QVERIFY(chooseService(dialog, "phantom"));
    auto* market = child<QComboBox>(dialog, "marketSelector");
    auto* field = child<QLineEdit>(dialog, "credential_address");
    auto* feedback = child<QLabel>(dialog, "phantomAddressFeedback");
    auto* change = child<QPushButton>(dialog, "switchPhantomAddressMarket");
    auto* save = child<QPushButton>(dialog, "saveAccount");
    QSignalSpy submitted(&dialog, &AccountDialog::submitted);
    if (QGuiApplication::platformName() == "offscreen") {
        // Exercise the native paste entry point without modifying the user's system clipboard.
        QGuiApplication::clipboard()->setText(eth);
        field->paste();
        QGuiApplication::clipboard()->clear();
    } else field->insert(eth);
    QCOMPARE(market->currentData().toString(), QString("solana"));
    QVERIFY(!save->isEnabled());
    QVERIFY(change->isVisible());
    QVERIFY(feedback->text().contains("Ethereum") && change->text().contains("Hyperliquid"));
    change->click();
    QCOMPARE(market->currentData().toString(), QString("hyperliquid_perps"));
    QCOMPARE(field->text(), eth);
    QVERIFY(save->isEnabled() && !change->isVisible());
    QVERIFY(feedback->text().contains(QStringLiteral("Ethereum 주소 형식이 맞아요")));
    field->setText(sol);
    QVERIFY(!save->isEnabled());
    QVERIFY(change->isVisible() && change->text().contains("Solana"));
    market->setCurrentIndex(market->findData("solana"));
    QCOMPARE(field->text(), sol);
    QVERIFY(save->isEnabled() && !change->isVisible());
    QCOMPARE(submitted.count(), 0);
    save->click();
    QCOMPARE(submitted.count(), 1);
    const auto draft = submitted.first().first().value<AccountDraft>();
    QCOMPARE(draft.market, QString("solana"));
    QCOMPARE(draft.credentials.value("address"), sol);
}

void ConnectionFormTest::phantomReplacementKeepsTheSelectedMarket() {
    AccountDialog dialog(true);
    AccountDraft old; old.provider = "phantom"; old.market = "solana";
    old.environment = "mainnet"; old.currency = "SOL";
    dialog.setReplacement(old);
    dialog.show();
    child<QLineEdit>(dialog, "credential_address")->setText("0x1111111111111111111111111111111111111111");
    QVERIFY(child<QLabel>(dialog, "phantomAddressFeedback")->text().contains("Ethereum"));
    QVERIFY(!child<QComboBox>(dialog, "marketSelector")->isEnabled());
    QVERIFY(!child<QPushButton>(dialog, "switchPhantomAddressMarket")->isVisible());
    QVERIFY(!child<QPushButton>(dialog, "saveAccount")->isEnabled());
}

void ConnectionFormTest::phantomPerpsAddressIsRequiredAndValidated_data() {
    QTest::addColumn<QString>("address");
    QTest::addColumn<bool>("valid");
    QTest::newRow("empty") << QString{} << false;
    QTest::newRow("solana-address") << QString("11111111111111111111111111111111") << false;
    QTest::newRow("short") << QString("0x1234") << false;
    QTest::newRow("no-prefix") << QString("1111111111111111111111111111111111111111") << false;
    QTest::newRow("not-hex") << QString("0xgggggggggggggggggggggggggggggggggggggggg") << false;
    QTest::newRow("zero-address") << QString("0x0000000000000000000000000000000000000000") << false;
    QTest::newRow("valid") << QString("0x1111111111111111111111111111111111111111") << true;
    QTest::newRow("mixed-case") << QString("0xaBcDeFaBcDeFaBcDeFaBcDeFaBcDeFaBcDeFaBcD") << true;
}

void ConnectionFormTest::phantomPerpsAddressIsRequiredAndValidated() {
    QFETCH(QString, address);
    QFETCH(bool, valid);
    AccountDialog dialog(true);
    dialog.show();
    QCoreApplication::processEvents();
    QVERIFY(chooseService(dialog, "phantom"));
    auto* market = child<QComboBox>(dialog, "marketSelector");
    QVERIFY(market->findData("hyperliquid_perps") >= 0);
    market->setCurrentIndex(market->findData("hyperliquid_perps"));
    child<QLineEdit>(dialog, "accountAlias")->setText(QStringLiteral("가짜 Hyperliquid 조회"));
    child<QLineEdit>(dialog, "credential_address")->setText(address);
    QSignalSpy submitted(&dialog, &AccountDialog::submitted);
    auto* save = child<QPushButton>(dialog, "saveAccount");
    QCOMPARE(save->isEnabled(), valid);
    QTest::mouseClick(save, Qt::LeftButton);
    QCOMPARE(submitted.size(), valid ? 1 : 0);
    if (valid) {
        const auto draft = submitted.first().first().value<AccountDraft>();
        QCOMPARE(draft.provider, QString("phantom"));
        QCOMPARE(draft.market, QString("hyperliquid_perps"));
        QCOMPARE(draft.environment, QString("mainnet"));
        QCOMPARE(draft.currency, QString("USDC"));
        QCOMPARE(draft.credentials.value("address"), address);
    }
}

QTEST_MAIN(ConnectionFormTest)
#include "tst_connection_form.moc"
