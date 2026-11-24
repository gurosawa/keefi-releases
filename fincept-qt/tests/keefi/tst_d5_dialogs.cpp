#include "keefi/AccountDialog.h"
#include "keefi/AssetsLedger.h"
#include "keefi/ChartShareDialog.h"
#include "keefi/KeefiFonts.h"
#include "keefi/KeefiTheme.h"
#include "keefi/MarketCandles.h"
#include "keefi/RecordTransfer.h"
#include "keefi/RecordTransferDialog.h"
#include "keefi/PressRevealButton.h"
#include "keefi/SegmentedComboBox.h"
#include "keefi/Vault.h"
#include "keefi/VaultDialog.h"
#include "keefi/VaultGate.h"
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPaintEngine>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QTextDocument>
#include <QStyleOptionViewItem>
using namespace keefi;
namespace {
const QString SyntheticPassword = QStringLiteral("Synthetic D5 Vault! 2026");
QString undersizedAction(QWidget& owner, int actionSize = 44, int inputHeight = 44) {
    for(auto* action:owner.findChildren<QAbstractButton*>()) {
        if(!action->isVisibleTo(&owner)||(!qobject_cast<QPushButton*>(action)&&!qobject_cast<QCheckBox*>(action)&&!qobject_cast<PressRevealButton*>(action)))continue;
        if(action->width()<actionSize||action->height()<actionSize)return action->objectName()+" "+action->text()+QString(" %1x%2").arg(action->width()).arg(action->height());
    }
    for(auto* combo:owner.findChildren<QComboBox*>())if(combo->isVisibleTo(&owner)&&(combo->width()<actionSize||combo->height()<actionSize))return combo->objectName()+QString(" %1x%2").arg(combo->width()).arg(combo->height());
    for(auto* input:owner.findChildren<QLineEdit*>())if(input->isVisibleTo(&owner)&&input->height()<inputHeight)return input->objectName()+QString(" input height %1").arg(input->height());
    return {};
}
bool hasRole(const QFont& actual, FontRole role) {
    const auto expected = KeefiFonts::font(role);
    return actual.family() == expected.family() && actual.pixelSize() == expected.pixelSize()
        && actual.weight() == expected.weight();
}
// Record actual text shaping at the paint boundary, rather than assuming a
// widget's base font also describes each numeric run in its item delegate.
class TextPaintEngine final : public QPaintEngine {
public:
    struct Run { QString text; QFont font; };
    QList<Run> runs;
    bool begin(QPaintDevice*) override { return true; }
    bool end() override { return true; }
    Type type() const override { return User; }
    void updateState(const QPaintEngineState&) override {}
    void drawPixmap(const QRectF&, const QPixmap&, const QRectF&) override {}
    void drawPath(const QPainterPath&) override {}
    void drawPolygon(const QPointF*, int, PolygonDrawMode) override {}
    void drawTextItem(const QPointF&, const QTextItem& item) override { runs.append({item.text(), item.font()}); }
};
class TextPaintDevice final : public QPaintDevice {
public:
    mutable TextPaintEngine engine;
    QPaintEngine* paintEngine() const override { return &engine; }
    int metric(PaintDeviceMetric value) const override {
        switch (value) {
        case PdmWidth: return 400;
        case PdmHeight: return 80;
        case PdmDepth: return 32;
        case PdmNumColors: return 0;
        case PdmDevicePixelRatio: return 1;
        case PdmDevicePixelRatioScaled: return int(devicePixelRatioFScale());
        default: return 96;
        }
    }
};
QList<TextPaintEngine::Run> paintedCell(QTableWidget& table, int column) {
    TextPaintDevice device; QPainter painter(&device);
    QStyleOptionViewItem option; option.initFrom(&table); option.widget = &table;
    option.rect = QRect(0, 0, 400, 40); option.font = table.font();
    const auto index = table.model()->index(0, column);
    table.itemDelegateForIndex(index)->paint(&painter, option, index);
    painter.end(); return device.engine.runs;
}
QList<TextPaintEngine::Run> paintedLabel(QLabel& label) {
    TextPaintDevice device; label.resize(label.sizeHint()); label.render(&device);
    return device.engine.runs;
}
bool paintsWithRole(const QList<TextPaintEngine::Run>& runs, const QString& text, FontRole role) {
    for (const auto& run : runs)
        if (run.text.contains(text) && hasRole(run.font, role)) return true;
    return false;
}
bool seedSyntheticRecords(Vault& vault, const QString& directory, QString* error) {
    AssetsLedger ledger(&vault, directory + "/assets.keefi");
    if (!ledger.open(true, error)) return false;
    AssetSnapshot row;
    row.id = "synthetic-d5-snapshot"; row.accountScope = "SYNTHETIC_D5_SCOPE";
    row.provider = "binance"; row.market = "spot"; row.environment = "live";
    row.timeMs = 1792713600000LL; row.balanceComplete = row.reconciled = true;
    row.evidence = {"SYNTHETIC_D5_SOURCE", "https://example.invalid/d5", "SYNTHETIC_D5_SCOPE", row.timeMs, row.timeMs, true};
    AssetComponent component; component.id = "synthetic-d5-asset"; component.asset = "SYN";
    component.quantity = "123.45"; component.evidence = row.evidence; row.components = {component};
    if (!ledger.append({row}, {}, {}, error)) return false;
    RecordTransfer transfer(vault); RecordTransferBundle bundle;
    return transfer.initialize(error) && transfer.collect({}, &ledger, nullptr, &bundle, error)
        && transfer.commit(bundle, transfer.generation(), nullptr, error);
}
ChartShareSnapshot publicSyntheticChart() {
    constexpr qint64 start = 1792713600000LL, minute = 60000;
    ChartShareSnapshot value; value.identity = {"coin", "upbit", "KRW-SYN"};
    value.currency = "KRW"; value.intervalMs = minute; value.displayTimeZoneId = "Asia/Seoul";
    for (int i = 0; i < 30; ++i)
        value.closed.append({start+i*minute, start+(i+1)*minute-1, "100", "102", "99", i%2 ? "101" : "100", "2"});
    value.calculatedThroughMs = start+30*minute; value.asOfMs = value.calculatedThroughMs+1000;
    value.viewport = {start, start+29*minute}; value.studies = keefiChartStudies(value.closed, minute);
    value.source.provider = ChartShareSource::Upbit;
    value.source.canonicalUrl = MarketCandles::request(value.identity.key(), minute).url.toString();
    value.source.receivedAtMs = value.calculatedThroughMs+500;
    value.visiblePublicLayers = {"volume"}; value.availability = ChartShareAvailability::Ready;
    return value;
}
bool capture(QWidget& widget, const QString& prefix) {
    const auto output = qEnvironmentVariable("KEEFI_D5_RENDER_DIR");
    if (output.isEmpty()) return true;
    if (!QDir().mkpath(output)) return false;
    widget.show(); QTest::qWait(35);
    const auto name = prefix + (KeefiTheme::instance()->isDark() ? "-dark.png" : "-light.png");
    return widget.grab().save(QDir(output).filePath(name));
}
}
class D5DialogsTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QVERIFY(KeefiFonts::registerBundledFonts());
        KeefiTheme::instance()->setSettings(nullptr);
        KeefiTheme::instance()->setMode(ThemeMode::Light);
        qApp->setStyleSheet(KeefiTheme::localStyleSheet());
    }
    void closingVaultDialogClearsSecretsWithoutResizing_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("unlock")<<int(VaultDialog::Mode::Unlock);
        QTest::newRow("create")<<int(VaultDialog::Mode::Create);
        QTest::newRow("change")<<int(VaultDialog::Mode::ChangePassword);
    }
    void closingVaultDialogClearsSecretsWithoutResizing() {
        QFETCH(int, mode);
        VaultDialog dialog(static_cast<VaultDialog::Mode>(mode));
        class ClosingEvents final : public QObject {
        public:
            bool closing=false;int resizes=0;
            bool eventFilter(QObject*,QEvent* event) override {
                if(closing&&event->type()==QEvent::Resize)++resizes;
                return false;
            }
        } events;
        dialog.installEventFilter(&events);
        int finished=0;
        connect(&dialog,&QDialog::finished,&dialog,[&]{
            ++finished;
            for(auto* edit:dialog.findChildren<QLineEdit*>()){
                QVERIFY(edit->text().isEmpty());
                QCOMPARE(edit->echoMode(),QLineEdit::Password);
                QVERIFY(!edit->isUndoAvailable());
            }
        });
        connect(&dialog,&VaultDialog::submitted,&dialog,[&]{
            events.closing=true;dialog.accept();events.closing=false;
        });
        for(int attempt=0;attempt<2;++attempt){
            dialog.show();QTest::qWait(20);
            for(auto* edit:dialog.findChildren<QLineEdit*>())edit->setText(SyntheticPassword);
            auto* submit=dialog.findChild<QPushButton*>("submitVault");
            QVERIFY(submit);QVERIFY(submit->isEnabled());
            QTest::mouseClick(submit,Qt::LeftButton);
            QVERIFY(!dialog.isVisible());QCOMPARE(finished,attempt+1);
            QCOMPARE(events.resizes,0);
        }
        dialog.show();QTest::qWait(20);
        for(auto* edit:dialog.findChildren<QLineEdit*>())edit->setText(SyntheticPassword);
        dialog.setError("SYNTHETIC_RETRY");
        QVERIFY(dialog.isVisible());
        for(auto* edit:dialog.findChildren<QLineEdit*>())QVERIFY(edit->text().isEmpty());
        events.closing=true;dialog.reject();events.closing=false;
        QCOMPARE(finished,3);QCOMPARE(events.resizes,0);
    }
    void accountWizardUsesCompactTargetsAndMarksTheCurrentStep() {
        AccountDialog account(true);
        account.show();
        // Services are 48 px tiles; there is no search field or "next" button.
        auto* tile = account.findChild<QAbstractButton*>("accountService_binance");
        QVERIFY(tile);
        QCOMPARE(tile->height(), 48);
        QVERIFY(!account.findChild<QLineEdit*>("accountServiceSearch"));
        QVERIFY(!account.findChild<QPushButton*>("accountServiceNext"));
        const auto currentStep = [&] {
            for (auto* label : account.findChildren<QLabel*>())
                if (label->property("accountCurrentStep").toBool()) return label->text();
            return QString{};
        };
        QCOMPARE(currentStep(), QStringLiteral("서비스 선택"));
        auto* provider = account.findChild<QComboBox*>("providerSelector");
        QVERIFY(provider);
        provider->setCurrentIndex(provider->findData("binance"));
        QCOMPARE(currentStep(), QStringLiteral("연결 정보"));
        auto* secret = account.findChild<QLineEdit*>("credential_secret");
        auto* reveal = account.findChild<PressRevealButton*>("reveal_secret");
        QVERIFY(secret && reveal);
        QTRY_COMPARE(secret->height(), 32);
        QTRY_COMPARE(reveal->height(), 28);
        account.showConnectionResult("SYNTHETIC_NO_CONNECTION", nullptr);
        QCOMPARE(currentStep(), QStringLiteral("연결 확인"));
    }
    void trimNoticeKeepsDialogHeightAndSubmitButtonPosition() {
        AccountDialog account(true);
        account.show();
        auto* tile = account.findChild<QAbstractButton*>("accountService_binance");
        QVERIFY(tile);
        tile->click();
        QCoreApplication::processEvents();
        auto* key = account.findChild<QLineEdit*>("credential_api_key");
        auto* notice = account.findChild<QLabel*>("credentialTrimNotice");
        auto* submit = account.findChild<QPushButton*>("saveAccount");
        QVERIFY(key && notice && submit && submit->parentWidget());
        QVERIFY(notice->isHidden());
        // The buttons sit outside the scrolling page and never moved. What used to jump is
        // everything below the notice inside the page.
        auto* page = notice->parentWidget();
        QVERIFY(page);
        const int beforePageHeight = page->sizeHint().height();
        QList<QPair<QWidget*, int>> below;
        for (const char* name : {"storageHint", "openVaultForAccount", "accountStageStatus", "accountError"})
            if (auto* widget = account.findChild<QWidget*>(name); widget && widget->isVisible())
                below.append({widget, widget->mapTo(&account, QPoint()).y()});
        const int beforeHeight = account.height();
        const int beforeSubmitTop = submit->geometry().top();
        const int beforeBoxTop = submit->parentWidget()->geometry().top();
        const int beforeSubmitY = submit->mapTo(&account, QPoint()).y();
        key->setText(QStringLiteral("  FAKE-KEY  "));
        emit key->editingFinished();
        QCoreApplication::processEvents();
        QVERIFY(notice->isVisible());
        QCOMPARE(notice->text(), QStringLiteral("앞뒤 공백을 제거했습니다."));
        QCOMPARE(account.height(), beforeHeight);
        QCOMPARE(submit->geometry().top(), beforeSubmitTop);
        QCOMPARE(submit->parentWidget()->geometry().top(), beforeBoxTop);
        QCOMPARE(submit->mapTo(&account, QPoint()).y(), beforeSubmitY);
        QCOMPARE(page->sizeHint().height(), beforePageHeight);
        for (const auto& [widget, y] : below) QCOMPARE(widget->mapTo(&account, QPoint()).y(), y);
    }
    void guidanceUsesBodyRolesAndSecretsKeepTheirOriginalRules() {
        AccountDialog account(true); account.show();
        auto* guide = account.findChild<QLabel*>("providerGuide");
        QVERIFY(guide);
        QVERIFY(hasRole(guide->font(), FontRole::Secondary14));
        auto* alias = account.findChild<QLineEdit*>("accountAlias");
        QVERIFY(alias); QVERIFY(hasRole(alias->font(), FontRole::Body15));
        auto* secret = account.findChild<QLineEdit*>("credential_client_secret");
        QVERIFY(secret); QCOMPARE(secret->echoMode(), QLineEdit::Password);
        QCOMPARE(secret->maxLength(), 8193); QVERIFY(secret->text().isEmpty());
        QVERIFY(hasRole(secret->font(), FontRole::Body15));
        auto* provider=account.findChild<QComboBox*>("providerSelector");QVERIFY(provider);
        provider->setCurrentIndex(provider->findData("binance"));QTest::qWait(20);
        const auto formTarget=undersizedAction(account, 28, 32);QVERIFY2(formTarget.isEmpty(),qPrintable(formTarget));
        auto* market=account.findChild<SegmentedComboBox*>("marketSelector");QVERIFY(market);
        const auto segments=market->segmentRects();int target=-1;
        for(int row=0;row<segments.size();++row)if(!segments[row].isEmpty()){
            QVERIFY(segments[row].height()>=28);
            if(market->isItemEnabled(row)&&row!=market->currentIndex())target=row;
        }
        QVERIFY(target>=0);QTest::mouseClick(market,Qt::LeftButton,Qt::NoModifier,segments[target].bottomLeft()+QPoint(2,-2));
        QCOMPARE(market->currentIndex(),target);
        account.showConnectionResult("SYNTHETIC_D5_NO_CONNECTION", nullptr);
        auto* result = account.findChild<QLabel*>("accountConnectionResult"); QVERIFY(result);
        QTextDocument resultDocument; resultDocument.setHtml(result->text());
        QCOMPARE(resultDocument.toPlainText(), QString("조회 대기\n잔고 조회를 기다리고 있습니다.\n\n저장 완료"));
        QCOMPARE(result->textInteractionFlags(), Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        result->setSelection(0, QString("조회 대기").size());
        QCOMPARE(result->selectedText(), QString("조회 대기"));
        VaultDialog vault(VaultDialog::Mode::Create); vault.show();
        auto* requirements = vault.findChild<QLabel*>("passwordRequirements");
        QVERIFY(requirements); QVERIFY(hasRole(requirements->font(), FontRole::Secondary14));
        auto* password = vault.findChild<QLineEdit*>("newPassword");
        QVERIFY(password); QCOMPARE(password->echoMode(), QLineEdit::Password);
        QCOMPARE(password->maxLength(), int(Vault::MaxPasswordBytes)+1);
        QVERIFY(hasRole(password->font(), FontRole::Body15));
        QCOMPARE(vault.findChild<QPushButton*>("submitVault")->isEnabled(), false);
        ChartShareDialog share(publicSyntheticChart()); share.show();
        const auto privacy = share.findChild<QLabel*>("chartSharePrivacy");
        const auto status = share.findChild<QLabel*>("chartShareStatus");
        QVERIFY(privacy && status);
        QVERIFY(hasRole(privacy->font(), FontRole::Caption12));
        QVERIFY(hasRole(status->font(), FontRole::Secondary14));
        QCOMPARE(privacy->text(), QString("계좌 금액·수량 제외 · 로컬 저장만"));
        QTest::qWait(20);
        const auto accountTarget=undersizedAction(account, 28, 32),vaultTarget=undersizedAction(vault);
        QVERIFY2(accountTarget.isEmpty(),qPrintable(accountTarget));QVERIFY2(vaultTarget.isEmpty(),qPrintable(vaultTarget));
        VaultGate gate(false,{});gate.setSecurityReady(true);gate.resize(1440,900);gate.show();QTest::qWait(20);
        auto* embedded=gate.findChild<VaultDialog*>("vaultGateCreateForm");QVERIFY(embedded);
        for(auto* input:embedded->findChildren<QLineEdit*>())if(input->isVisibleTo(&gate))QVERIFY2(input->height()==32,qPrintable(input->objectName()));
        for(auto* reveal:embedded->findChildren<PressRevealButton*>())if(reveal->isVisibleTo(&gate))QCOMPARE(reveal->height(),28);
        QCOMPARE(embedded->findChild<QPushButton*>("submitVault")->height(),28);
    }
    void recordedCodesAndNumbersUseMonoWithoutChangingKoreanKinds() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        Vault vault(temporary.filePath("synthetic-d5-vault")); QVERIFY(vault.create(SyntheticPassword));
        QString error; QVERIFY2(seedSyntheticRecords(vault, temporary.path(), &error), qPrintable(error));
        RecordTransferDialog dialog(vault, nullptr, nullptr); dialog.openRecords();
        auto* handover=dialog.findChild<QLabel*>("recordHandoverScope");QVERIFY(handover);
        QVERIFY(!handover->text().contains(QStringLiteral("거래를 중지")));
        QVERIFY(!handover->text().contains(QStringLiteral("동시 주문")));
        QVERIFY(handover->text().contains(QStringLiteral("조회")));
        auto* table = dialog.findChild<QTableWidget*>("importedRecordTable");
        QVERIFY(table); QCOMPARE(table->rowCount(), 1);
        const auto counter = dialog.findChild<QLabel*>("recordPageState"); QVERIFY(counter);
        QCOMPARE(counter->text(), QString("1–1 / 1개 기록 · 페이지당 200개"));
        QCOMPARE(table->item(0, 0)->text(), QString("자산 스냅샷"));
        QCOMPARE(table->item(0, 5)->text(), QString("SYN"));
        QCOMPARE(table->item(0, 6)->text(), QString("123.45"));
        QVERIFY(paintsWithRole(paintedCell(*table, 4), "2026", FontRole::Number11));
        QVERIFY(paintsWithRole(paintedCell(*table, 5), "SYN", FontRole::Number11));
        QVERIFY(paintsWithRole(paintedCell(*table, 6), "123.45", FontRole::Number11));
        QVERIFY(paintsWithRole(paintedCell(*table, 0), "자산", FontRole::Body15));
        QVERIFY(paintsWithRole(paintedCell(*table, 8), "미상", FontRole::Body15));
        QVERIFY(paintsWithRole(paintedLabel(*counter), "200", FontRole::Number11));
        QVERIFY(paintsWithRole(paintedLabel(*counter), "기록", FontRole::Caption12));
        QCOMPARE(table->editTriggers(), QAbstractItemView::NoEditTriggers);
        dialog.show();QTest::qWait(20);const auto recordTarget=undersizedAction(dialog);QVERIFY2(recordTarget.isEmpty(),qPrintable(recordTarget));
        auto* tabs=dialog.findChild<QTabWidget*>();QVERIFY(tabs);tabs->setCurrentIndex(0);QTest::qWait(20);
        const auto exportTarget=undersizedAction(dialog);QVERIFY2(exportTarget.isEmpty(),qPrintable(exportTarget));
        tabs->setCurrentIndex(2);
        const auto before = table->item(0, 6)->text();
        KeefiTheme::instance()->setMode(ThemeMode::Dark);
        qApp->setStyleSheet(KeefiTheme::localStyleSheet());
        QCOMPARE(table->item(0, 6)->text(), before);
        QVERIFY(paintsWithRole(paintedCell(*table, 6), "123.45", FontRole::Number11));
        vault.lock(); QCOMPARE(table->rowCount(), 0);
    }
    void syntheticTwoThemeDialogRenders() {
        if (qEnvironmentVariable("KEEFI_D5_RENDER_DIR").isEmpty()) return;
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        Vault vault(temporary.filePath("synthetic-only-vault")); QVERIFY(vault.create(SyntheticPassword));
        QString error; QVERIFY2(seedSyntheticRecords(vault, temporary.path(), &error), qPrintable(error));
        AccountDialog account(true); VaultDialog create(VaultDialog::Mode::Create), change(VaultDialog::Mode::ChangePassword);
        RecordTransferDialog records(vault, nullptr, nullptr); ChartShareDialog share(publicSyntheticChart());
        records.openRecords();
        for (auto mode : {ThemeMode::Light, ThemeMode::Dark}) {
            KeefiTheme::instance()->setMode(mode); qApp->setStyleSheet(KeefiTheme::localStyleSheet());
            account.resize(600, 680); QVERIFY(capture(account, "d5-dialog-account-services"));
            auto* syntheticChoice = account.findChild<QAbstractButton*>("accountService_upbit"); QVERIFY(syntheticChoice);
            syntheticChoice->click();
            // Selecting a service changes only the existing wizard page. No input or submission is performed.
            QVERIFY(capture(account, "d5-dialog-account-form"));
            account.findChild<QPushButton*>("accountServiceBack")->click();
            create.resize(620, 500); QVERIFY(capture(create, "d5-dialog-vault-create"));
            change.resize(620, 560); QVERIFY(capture(change, "d5-dialog-vault-change"));
            records.resize(960, 760); records.findChild<QTabWidget*>()->setCurrentIndex(0);
            QVERIFY(capture(records, "d5-dialog-record-export"));
            records.findChild<QTabWidget*>()->setCurrentIndex(2); QVERIFY(capture(records, "d5-dialog-record-table"));
            share.resize(900, 740); QVERIFY(capture(share, "d5-dialog-chart-share"));
        }
        vault.lock();
    }
};
QTEST_MAIN(D5DialogsTest)
#include "tst_d5_dialogs.moc"
