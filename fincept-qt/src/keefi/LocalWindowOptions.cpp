#include "keefi/LocalWindow.h"
#include "keefi/ReleaseUpdater.h"
#include "keefi/LocalWindowHelpers_p.h"
#include "keefi/ProfileSettings.h"
#include "keefi/UsabilityPresentation.h"
#include "keefi/LocalCheckBox.h"
#include "keefi/Vault.h"
#include "keefi/PortfolioController.h"
#include "keefi/AccountReadAccess.h"
#include "keefi/LocalComboBox.h"
#include "keefi/MarketMetricsView.h"
#include "keefi/ChartAlertsPanel.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include <algorithm>
#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QSettings>
#include "keefi/MovementColorPolicy.h"
#include <QMessageBox>
#include <QFileDialog>
#include <QDir>
#include <QSaveFile>
#include <QStandardPaths>
#include <QJsonDocument>
#include "keefi/ResponseJournal.h"
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QTabWidget>
#include <QTimer>
#include <QStackedWidget>
#include <QScrollArea>
#include <QDateTime>
#include "keefi/PortfolioMoneyDisplay.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/ValuationBundle.h"
#include <QStyle>
#include <QAbstractButton>
#include <QComboBox>
#include <QCoreApplication>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>

namespace keefi {
using local_window_detail::textLabel;
using local_window_detail::ledgerTextLabel;
using local_window_detail::button;
using local_window_detail::displayChoice;
using local_window_detail::problemConnectionStates;

namespace {
QString agoText(qint64 ms) {
    if (ms <= 0) return QObject::tr("시각 없음");
    const auto seconds = qMax<qint64>(0, (QDateTime::currentMSecsSinceEpoch() - ms) / 1000);
    return seconds < 60 ? QObject::tr("방금") : seconds < 3600 ? QObject::tr("%1분 전").arg(seconds / 60) : QObject::tr("%1시간 전").arg(seconds / 3600);
}
} // namespace

void LocalWindow::refreshTrust() {
    if (!trustPill_) return;
    const bool readable = portfolio_ && securityReady_ && accountReadsAvailable(vault_);
    const auto states = readable ? portfolio_->connections() : QList<PortfolioConnection>{};
    auto* pages = findChild<QTabWidget*>("portfolioPages");
    const bool hasReadableAccount = std::any_of(states.cbegin(), states.cend(), [](const auto& c) {
        return !AccountModel::isProviderSuspended(c.account.provider);
    });
    if (!hasReadableAccount || (pages && pages->currentWidget() == connectionsPage_)) { trustPill_->hide(); return; }
    int problems = 0; qint64 latest = 0;
    for (const auto& c : states) {
        problems += !c.shared && problemConnectionStates().contains(c.state);
        latest = qMax(latest, c.receivedAtMs);
    }
    // Background re-reads every 20 s keep the last verdict; only a first read says "조회 중".
    int firstReads = 0;
    for (const auto& m : portfolioView_.missing) firstReads += m.active;
    // One verdict, counting both balance reads and pricing, so the pill never says
    // "all confirmed" while the total still leaves assets out.
    const int pending = portfolioView_.summary.unvaluedGroups + portfolioView_.summary.referenceGroups;
    QString text;
    if (problems) text = tr("● 확인 필요 %1").arg(problems);
    else if (portfolioInitialRefresh_) text = tr("● 업데이트 중");
    else if (firstReads) text = tr("● 조회 중");
    else if (pending) text = tr("● 확인된 금액만 표시");
    else text = tr("● 모두 확인") + (latest > 0 ? " · " + agoText(latest) : QString{});
    const QString tone = problems ? "warn" : portfolioInitialRefresh_ || firstReads || pending ? "info" : "ok";
    const bool toneChanged = trustPill_->property("tone").toString() != tone;
    if (!trustPill_->isHidden() && trustPill_->text() == text && !toneChanged) return;
    trustPill_->setText(text);
    if (toneChanged) {
        trustPill_->setProperty("tone", tone);
        trustPill_->style()->unpolish(trustPill_); trustPill_->style()->polish(trustPill_);
    }
    trustPill_->setMinimumSize(trustPill_->sizeHint());
    trustPill_->show();
}

void LocalWindow::showTrustProcess() {
    if (!portfolio_ || !securityReady_ || !accountReadsAvailable(vault_)) return;
    auto* popup = new QFrame(this, Qt::Popup);
    popup->setObjectName("trustProcess");
    popup->setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(popup);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);
    auto* title = textLabel(tr("숫자가 만들어진 과정"), popup);
    title->setProperty("type", "section");
    layout->addWidget(title);
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(6);
    const QStringList heads{tr("연결"), tr("① 잔고 조회"), tr("② 가격 평가"), tr("③ 합계")};
    for (int column = 0; column < heads.size(); ++column) {
        auto* head = textLabel(heads[column], popup);
        head->setProperty("type", "muted");
        grid->addWidget(head, 0, column);
    }
    int row = 1;
    for (const auto& c : portfolio_->connections()) {
        int fresh = 0, reference = 0, unvalued = 0;
        for (const auto& group : portfolioView_.groups)
            for (const auto& h : group.holdings)
                if (h.connectionId == c.account.id) {
                    fresh += h.state == PortfolioValueState::Fresh;
                    reference += h.state == PortfolioValueState::Reference;
                    unvalued += h.state == PortfolioValueState::Unvalued;
                }
        const bool done = c.state == "complete";
        const bool problem = problemConnectionStates().contains(c.state);
        const bool suspended = AccountModel::isProviderSuspended(c.account.provider);
        const QString balance = suspended ? tr("연결 비활성") : c.shared ? tr("공유 연결") : c.active ? tr("… 조회 중") : done ? tr("✓ %1").arg(agoText(c.receivedAtMs))
            : problem ? tr("× %1").arg(c.stage.isEmpty() ? c.state : c.stage) : tr("– 대기");
        const QString pricing = suspended ? tr("– 제외") : !done ? tr("– 대기") : reference || unvalued
            ? tr("! 최신 %1 · 참고 %2 · 미평가 %3").arg(fresh).arg(reference).arg(unvalued) : fresh ? tr("✓ 최신 %1").arg(fresh) : tr("✓ 평가할 자산 없음");
        const QString total = !done ? tr("– 제외") : reference || unvalued ? tr("일부 포함") : tr("✓ 포함");
        const QStringList cells{c.account.alias + " · " + displayChoice(c.account.market), balance, pricing, total};
        for (int column = 0; column < cells.size(); ++column) {
            auto* cell = textLabel(cells[column], popup);
            cell->setWordWrap(false);
            if (column && cells[column].startsWith("×")) cell->setProperty("tone", "error");
            else if (column && cells[column].startsWith("!")) cell->setProperty("tone", "warning");
            grid->addWidget(cell, row, column);
        }
        ++row;
    }
    layout->addLayout(grid);
    const auto pricing = portfolio_->pricingStatus().section('\n', 0, 0);
    auto* note = textLabel(tr("잔고 조회 → 가격 평가 → 합계 순서입니다. 한 단계라도 끝나지 않은 자산은 합계에 넣지 않고 따로 표시합니다.") +
        (pricing.isEmpty() ? QString{} : QStringLiteral("\n") + pricing), popup);
    note->setProperty("type", "muted");
    note->setMaximumWidth(560);
    layout->addWidget(note);
    auto* go = button(tr("계좌·연결에서 원인과 조치 보기"), "trustProcessConnections", popup);
    connect(go, &QPushButton::clicked, this, [this, popup] {
        if (auto* pages = findChild<QTabWidget*>("portfolioPages")) pages->setCurrentWidget(connectionsPage_);
        popup->close();
    });
    auto* actions = new QHBoxLayout;
    auto* prices = button(tr("원화 평가 상세"), "trustProcessPrices", popup);
    prices->setEnabled(findChild<QPushButton*>("portfolioPriceDetails")->isEnabled());
    connect(prices, &QPushButton::clicked, this, [this, popup] { popup->close(); findChild<QPushButton*>("portfolioPriceDetails")->click(); });
    actions->addWidget(prices);
    actions->addStretch();
    actions->addWidget(go);
    layout->addLayout(actions);
    connect(&vault_, &Vault::aboutToLock, popup, &QWidget::close);
    popup->adjustSize();
    const auto anchor = trustPill_->mapToGlobal(QPoint(trustPill_->width(), trustPill_->height() + 6));
    popup->move(anchor.x() - popup->width(), anchor.y());
    popup->show();
}

QJsonObject LocalWindow::valuationDiagnostics() {
    if (!portfolio_ || !accountReadsAvailable(vault_)) return {};
    // Value at the moment of export, not at the last input change.
    portfolioRevision_.clear();
    refreshPortfolio();
    // Until the first read after unlock settles, the screen shows loading or a dated
    // saved view whose inputs are not the current ones; exporting would mix the two.
    if (portfolioInitialRefresh_ || lastValuation_.asOfMs <= 0) return {};
    return valuationBundle(lastValuation_, smallThresholdKrw_, ResponseJournal::entries(), ResponseJournal::enabled(),
                           QCoreApplication::applicationName(), QCoreApplication::applicationVersion());
}

void LocalWindow::exportValuationDiagnostics(QWidget* parent) {
    if (exportDialog_) { exportDialog_->raise(); return; }
    // One export is one unlocked session: every modal step can deliver a lock, and
    // nothing is written unless the same generation is still open afterwards.
    const auto generation = exportGeneration_;
    const auto alive = [this, generation] {
        return generation == exportGeneration_ && securityReady_ && portfolio_ && accountReadsAvailable(vault_);
    };
    auto bundle = valuationDiagnostics();
    if (bundle.isEmpty()) {
        QMessageBox::information(parent, tr("평가 진단"), portfolioInitialRefresh_
            ? tr("잠금 해제 뒤 현재 잔고와 가격을 확인하는 중이에요. 마지막 확인 화면이 현재 평가로 바뀐 뒤에 내보낼 수 있어요.")
            : tr("금고를 열고 자산 평가가 한 번 끝난 뒤에 내보낼 수 있어요."));
        return;
    }
    const bool raw = bundle.value("raw_responses").toObject().value("recording").toBool();
    QMessageBox confirm(QMessageBox::Question, tr("평가 진단 내보내기"),
        tr("이 파일은 암호화되지 않아요. 잔고와 평가액%1이 그대로 들어가요. API 키와 금고 암호는 들어가지 않아요.\n"
           "대사가 끝나면 파일을 지워 주세요. 계속할까요?").arg(raw ? tr(", 거래소 원시 응답") : QString{}),
        QMessageBox::Yes | QMessageBox::Cancel, parent);
    confirm.setObjectName("valuationExportConfirm");
    confirm.setDefaultButton(QMessageBox::Cancel);
    exportDialog_ = &confirm;
    const bool agreed = confirm.exec() == QMessageBox::Yes;
    exportDialog_.clear();
    if (!agreed || !alive()) return;
    const auto asOf = QDateTime::fromMSecsSinceEpoch(qint64(bundle.value("as_of_ms").toDouble()));
    QFileDialog chooser(parent, tr("평가 진단 저장"),
        QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath("keefi-bundle-" + asOf.toString("yyyyMMdd-HHmmss") + ".json"),
        tr("JSON (*.json)"));
    chooser.setObjectName("valuationExportFile");
    chooser.setAcceptMode(QFileDialog::AcceptSave);
    chooser.setFileMode(QFileDialog::AnyFile);
    chooser.setDefaultSuffix("json");
    exportDialog_ = &chooser;
    const bool chosen = chooser.exec() == QDialog::Accepted && !chooser.selectedFiles().isEmpty();
    exportDialog_.clear();
    if (!chosen || !alive()) return; // The bundle copy ends with this function.
    const auto path = chooser.selectedFiles().first();
    QSaveFile file(path);
    if (!alive() || !file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(bundle).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
        if (alive()) QMessageBox::warning(parent, tr("평가 진단"), tr("파일을 저장하지 못했어요. 다른 위치를 골라 주세요."));
        return;
    }
    if (!QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        QMessageBox::warning(parent, tr("평가 진단"), tr("저장했지만 파일 권한을 나만 읽기로 제한하지 못했어요. 파일을 직접 확인해 주세요.\n%1").arg(path));
        return;
    }
    QMessageBox::information(parent, tr("평가 진단"), tr("%1 기준 평가를 저장했어요.\n%2").arg(asOf.toString("yyyy-MM-dd HH:mm:ss"), path));
}

void LocalWindow::openOptions() {
    openOptionsPage(optionsPage_);
}

void LocalWindow::openOptionsPage(const QString& page, const QString& listId) {
    const QString requested = page.isEmpty() ? QString("optionsGeneral") : page;
    if (optionsDialog_) {
        auto* choice=optionsDialog_->findChild<QPushButton*>(requested+"Tab");
        if(choice)choice->click();
        if(requested=="optionsAlerts"&&chartAlertsPanel_)chartAlertsPanel_->selectSettingsList(listId);
        optionsDialog_->raise();optionsDialog_->activateWindow();
        if(choice)choice->setFocus(Qt::OtherFocusReason);
        return;
    }
    // AXPress opens a button without first moving keyboard focus to it. Return
    // to that opener; the Preferences shortcut instead preserves its origin.
    QPointer<QWidget> returnFocus = qobject_cast<QAbstractButton*>(sender());
    if (!returnFocus) returnFocus = focusWidget();
    auto* dialog = new QDialog(this);
    optionsDialog_ = dialog;
    dialog->setObjectName("optionsDialog");
    dialog->setWindowTitle(tr("옵션"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::finished, this, [this, returnFocus] {
        QTimer::singleShot(0, this, [returnFocus] {
            if (returnFocus && returnFocus->isVisible() && returnFocus->isEnabled()) {
                returnFocus->window()->activateWindow();
                returnFocus->setFocus(Qt::OtherFocusReason);
            }
        });
    });
    dialog->resize(760, 560);
    dialog->setMinimumSize(560, 360);
    auto* root = new QHBoxLayout(dialog);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(16);
    // These static pages need no item model. Native buttons avoid Qt 6.8.3's
    // Cocoa table/cell AX teardown path (QTBUG-134784) without disabling a11y.
    auto* nav = new QWidget(dialog);
    nav->setObjectName("optionsNav");
    nav->setAccessibleName(tr("옵션 페이지"));
    nav->setFixedWidth(150);
    auto* navLayout = new QVBoxLayout(nav);
    navLayout->setContentsMargins(6, 6, 6, 6);
    navLayout->setSpacing(0);
    auto* navGroup = new QButtonGroup(nav);
    navGroup->setExclusive(true);
    setThemeStyle(nav,
        "QWidget#optionsNav{background:${bg};border:1px solid ${rule};border-radius:12px;}"
        "QPushButton[optionsPage=\"true\"]{background:transparent;color:${text2};border:1px solid transparent;border-radius:8px;padding:0 12px;text-align:left;min-height:42px;}"
        "QPushButton[optionsPage=\"true\"]:checked{background:${surface};color:${text};}"
        "QPushButton[optionsPage=\"true\"]:hover{background:${surface};color:${text};}"
        "QPushButton[optionsPage=\"true\"]:focus{border-color:${accent};}");
    auto* stack = new QStackedWidget(dialog);
    stack->setObjectName("optionsPages");
    root->addWidget(nav);
    root->addWidget(stack, 1);
    const auto addPage = [nav, navLayout, navGroup, stack](const QString& title, const char* name) {
        auto* choice = new QPushButton(title, nav);
        choice->setObjectName(QString::fromLatin1(name) + "Tab");
        choice->setProperty("optionsPage", true);
        choice->setCheckable(true);
        choice->setAutoExclusive(true);
        choice->setAutoDefault(false);
        choice->setFocusPolicy(Qt::StrongFocus);
        choice->setFont(KeefiFonts::font(FontRole::Body15));
        choice->setMinimumHeight(44);
        navGroup->addButton(choice, stack->count());
        navLayout->addWidget(choice);
        // Enlarged controls must stay reachable in the 640x420 settings window.
        // Only the active page scrolls; the navigation and dialog stay fixed.
        auto* scroll = new QScrollArea(stack);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* page = new QWidget(scroll);
        page->setObjectName(name);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(4, 0, 4, 0);
        layout->setSpacing(10);
        auto* heading = textLabel(title, page);
        heading->setProperty("type", "section");
        layout->addWidget(heading);
        scroll->setWidget(page);
        stack->addWidget(scroll);
        return layout;
    };
    const auto note = [](const QString& text, QWidget* parent) {
        auto* label = ledgerTextLabel(text, FontRole::Secondary14, parent);
        label->setProperty("type", "muted");
        return label;
    };
    // Each control mirrors the window's own checkbox or button, which stays the
    // single source of state and of whether the action is allowed right now.
    const auto mirrorCheck = [dialog](QCheckBox* source, const char* name) {
        auto* box = new LocalCheckBox(source->text(), dialog);
        box->setObjectName(name);
        box->setChecked(source->isChecked());
        box->setEnabled(source->isEnabled());
        box->setToolTip(source->toolTip());
        connect(box, &QCheckBox::toggled, source, &QCheckBox::setChecked);
        connect(source, &QCheckBox::toggled, box, [box](bool on) { const QSignalBlocker block(box); box->setChecked(on); });
        return box;
    };
    QList<QPair<QPushButton*, QPushButton*>> mirrored;
    const auto mirrorButton = [dialog, &mirrored](QPushButton* source, const char* name, bool closeFirst) {
        auto* item = button(source->text(), name, dialog);
        item->setProperty("compact", true);
        connect(item, &QPushButton::clicked, dialog, [dialog, source, closeFirst] {
            if (closeFirst) dialog->close(); // Vault and record windows open on the main window.
            source->click();
        });
        mirrored.append(qMakePair(source, item));
        return item;
    };
    {
        auto* page = addPage(tr("일반"), "optionsGeneral");
        page->addWidget(mirrorCheck(background_, "optionBackground"));
        page->addWidget(note(tr("창을 닫으면 메뉴 막대 아이콘으로 계속 실행해요."), dialog));
        page->addWidget(mirrorCheck(keepAwake_, "optionKeepAwake"));
        page->addWidget(note(keepAwake_->toolTip(), dialog));
        if (releaseUpdater_) {
            page->addSpacing(12);
            auto* status = note({}, dialog);
            status->setObjectName("releaseUpdateStatus");
            page->addWidget(status);
            auto* row = new QHBoxLayout;
            auto* check = button(tr("업데이트 확인"), "checkReleaseUpdate", dialog);
            auto* restart = button(tr("업데이트 후 재시작"), "restartForUpdate", dialog);
            row->addWidget(check);
            row->addWidget(restart);
            row->addStretch();
            page->addLayout(row);
            const auto refresh = [this, status, check, restart] {
                using State = ReleaseUpdater::State;
                const auto state = releaseUpdater_->state();
                status->setText(releaseUpdater_->status());
                check->setEnabled(state != State::Checking && state != State::Downloading && state != State::Ready);
                restart->setEnabled(state == State::Available);
            };
            connect(releaseUpdater_, &ReleaseUpdater::changed, dialog, refresh);
            connect(check, &QPushButton::clicked, releaseUpdater_, &ReleaseUpdater::check);
            connect(restart, &QPushButton::clicked, releaseUpdater_, &ReleaseUpdater::download);
            refresh();
        }
        page->addStretch();
    }
    {
        auto* page = addPage(tr("표시"), "optionsDisplay");
        page->addWidget(new QLabel(tr("시장 요약"),dialog));
        for(const auto& choice:QList<QPair<QString,QString>>{{"breadth",tr("강세 비율")},{"relative",tr("BTC 대비")}}){
            auto* source=findChild<QCheckBox*>("exploreObservation_"+choice.first);if(!source)continue;
            auto* control=new LocalCheckBox(choice.second,dialog);control->setObjectName("optionExploreObservation_"+choice.first);control->setChecked(source->isChecked());
            connect(control,&QCheckBox::toggled,source,&QCheckBox::setChecked);
            connect(source,&QCheckBox::toggled,control,[control](bool enabled){const QSignalBlocker block(control);control->setChecked(enabled);});page->addWidget(control);
        }
        page->addWidget(note(tr("강세 비율은 시장 요약에, BTC 대비는 선택 종목 상세에 표시해요. 기준 자료가 없으면 미확인으로 남겨요."),dialog));
        page->addWidget(new QLabel(usabilityText(UsabilityText::MoneyCurrencyLabel),dialog));
        auto* currency=new LocalComboBox(dialog);currency->setObjectName("optionMoneyCurrency");currency->setAccessibleName(usabilityText(UsabilityText::MoneyCurrencyLabel));
        currency->addItem(usabilityText(UsabilityText::MoneyCurrencyKrw),"KRW");currency->addItem(usabilityText(UsabilityText::MoneyCurrencyUsd),"USD");
        currency->setCurrentIndex(currency->findData(moneyCurrency_==MoneyCurrency::Usd?"USD":"KRW"));page->addWidget(currency);
        connect(currency,qOverload<int>(&QComboBox::currentIndexChanged),this,[this,currency]{
            const auto value=currency->currentData().toString();moneyCurrency_=value=="USD"?MoneyCurrency::Usd:MoneyCurrency::Krw;
            if(chartSettings_)chartSettings_->setValue("display/moneyCurrency/v1",value);else ProfileSettings().setValue("display/moneyCurrency/v1",value);
            refreshMoneyPresentation();
        });
        const auto displayChoice = [dialog](const QString& label, const char* name, QPushButton* source) {
            auto* box = new LocalCheckBox(label, dialog);
            box->setObjectName(name);
            box->setChecked(source->isChecked());
            connect(box, &QCheckBox::toggled, source, &QPushButton::setChecked);
            connect(source, &QPushButton::toggled, box, [box](bool checked) {
                const QSignalBlocker block(box); box->setChecked(checked);
            });
            return box;
        };
        page->addWidget(displayChoice(tr("소액 숨기기"), "optionHideSmall", hideSmall_));
        page->addWidget(note(tr("1 USDT 미만인 평가 가능 자산을 목록에서 숨겨요. 기준 가격을 확인한 뒤 적용하며 합계는 그대로예요."), dialog));
        page->addWidget(displayChoice(tr("미평가 숨기기"), "optionHideUnvalued", hideUnvalued_));
        page->addWidget(note(tr("가격이 없는 자산을 목록에서 숨겨요. 부분 평가·계좌 조회 실패는 유지하며 합계와 진단 자료는 바뀌지 않아요."), dialog));
        auto* movement=new LocalComboBox(dialog);movement->setObjectName("optionMovementColors");movement->setAccessibleName(tr("상승·하락 표시 색"));
        movement->addItem(tr("상승 초록 · 하락 빨강"),int(MovementScheme::GreenRed));movement->addItem(tr("한국식: 상승 빨강 · 하락 파랑"),int(MovementScheme::KoreanRedBlue));movement->setCurrentIndex(movement->findData(int(movementColors_->scheme())));
        page->addWidget(new QLabel(tr("상승·하락 색"),dialog));page->addWidget(movement);page->addWidget(note(tr("캔들·등락·작은 차트·배지·앱 알림에 함께 적용해요. ▲▼ 기호와 위험 표시 색은 유지해요."),dialog));
        connect(movement,qOverload<int>(&QComboBox::currentIndexChanged),this,[this,movement]{movementColors_->setScheme(MovementScheme(movement->currentData().toInt()));});
        connect(movementColors_,&MovementColorPolicy::changed,movement,[this,movement]{const QSignalBlocker block(movement);movement->setCurrentIndex(movement->findData(int(movementColors_->scheme())));});
        page->addStretch();
    }
    {
        auto* page = addPage(tr("보안"), "optionsSecurity");
        auto* state = note(vaultStatus_->text(), dialog);
        state->setObjectName("optionVaultState");
        page->addWidget(state);
        auto* row = new QHBoxLayout;
        row->addWidget(mirrorButton(lockVault_, "optionLockVault", false));
        row->addWidget(mirrorButton(changePassword_, "optionChangePassword", true));
        row->addStretch();
        page->addLayout(row);
        page->addWidget(note(tr("PC를 잠그면 금고도 함께 잠겨요. 계좌 키는 이 PC의 암호화 금고에만 있어요."), dialog));
        page->addSpacing(12);
        auto* erase = button(tr("금고 지우기"), "optionEraseVault", dialog);
        erase->setProperty("compact", true);
        erase->setProperty("danger", true);
        const auto syncErase = [this, erase] { erase->setEnabled(securityReady_ && vault_.exists()); };
        syncErase();
        connect(&vault_, &Vault::stateChanged, erase, syncErase);
        connect(erase, &QPushButton::clicked, this, [this, dialog] { dialog->close(); confirmEraseVault(); });
        auto* eraseRow = new QHBoxLayout;
        eraseRow->addWidget(erase);
        eraseRow->addStretch();
        page->addLayout(eraseRow);
        page->addWidget(note(tr("암호를 잊어 금고를 열 수 없을 때 사용해요. 저장한 계좌 연결과 이 PC의 암호화 기록이 모두 지워져요."), dialog));
        page->addStretch();
        connect(&vault_, &Vault::stateChanged, state, [this, state] { state->setText(vaultStatus_->text()); });
    }
    {
        auto* page = addPage(tr("기록"), "optionsRecords");
        auto* row = new QHBoxLayout;
        row->addWidget(mirrorButton(findChild<QPushButton*>("openAssets"), "optionOpenAssets", true));
        row->addWidget(mirrorButton(findChild<QPushButton*>("openRecordTransfer"), "optionRecordTransfer", true));
        row->addStretch();
        page->addLayout(row);
        page->addWidget(note(tr("자산 변화 기록을 보거나, 암호화된 기록을 다른 PC로 옮겨요."), dialog));
        page->addSpacing(12);
        auto* recordResponses = new LocalCheckBox(tr("원시 응답 기록 · 이번 실행에서만, 잠그면 지움"), dialog);
        recordResponses->setObjectName("optionRecordResponses");
        recordResponses->setChecked(ResponseJournal::enabled());
        connect(recordResponses, &QCheckBox::toggled, this, [this](bool on) {
            ResponseJournal::setEnabled(on);
            // Fresh reads after switching on put every connection's response in the journal.
            if (on && portfolio_ && accountReadsAvailable(vault_)) portfolio_->refreshAll();
        });
        auto* exportValuation = button(tr("평가 진단 내보내기…"), "optionExportValuation", dialog);
        exportValuation->setProperty("compact", true);
        const auto syncDiagnostics = [this, recordResponses, exportValuation] {
            const bool open = portfolio_ && accountReadsAvailable(vault_);
            exportValuation->setEnabled(open);
            recordResponses->setEnabled(open);
            const QSignalBlocker block(recordResponses);
            recordResponses->setChecked(open && ResponseJournal::enabled());
        };
        syncDiagnostics();
        connect(&vault_, &Vault::stateChanged, exportValuation, syncDiagnostics);
        connect(exportValuation, &QPushButton::clicked, this, [this, dialog] { exportValuationDiagnostics(dialog); });
        page->addWidget(recordResponses);
        auto* diagnosticsRow = new QHBoxLayout;
        diagnosticsRow->addWidget(exportValuation);
        diagnosticsRow->addStretch();
        page->addLayout(diagnosticsRow);
        page->addWidget(note(tr("거래소 잔고와 맞춰 보기 위한 진단 파일이에요. 지금 화면의 평가 입력과 결과(잔고·가격·평가액)를 "
                                "암호화하지 않은 JSON으로 저장해요. API 키와 금고 암호는 들어가지 않아요. "
                                "원시 응답 기록을 켜면 그 뒤에 받은 응답 본문도 함께 들어가요."), dialog));
        page->addStretch();
    }
    {
        auto* page = addPage(tr("정보"), "optionsAbout");
        page->addWidget(note(tr("실계좌 수량·평가 대조 검증은 진행 중입니다."), dialog));
        auto* version=textLabel(tr("%1 %2").arg(QCoreApplication::applicationName(), QCoreApplication::applicationVersion()), dialog, "optionVersion");
        version->setFont(KeefiFonts::font(FontRole::Number11));page->addWidget(version);
        page->addWidget(note(tr("내 PC에서 조회하는 자산과 계좌"), dialog));
        auto* row = new QHBoxLayout;
        row->addWidget(mirrorButton(findChild<QPushButton*>("quitApplication"), "optionQuit", true));
        row->addStretch();
        page->addLayout(row);
        page->addStretch();
    }
    {
        auto* page = addPage(tr("알림"), "optionsAlerts");
        if(chartAlertsPanel_)page->addWidget(chartAlertsPanel_->createSettingsPage(page->parentWidget(),listId));
        else page->addWidget(note(tr("알림 서비스를 준비하고 있어요."),dialog));
        page->addStretch();
    }
    {
        auto* page = addPage(tr("시장 지표 알림"), "optionsMarketAlerts");
        page->addWidget(note(tr("탐색 화면의 시장 지표 알림을 설정해요."),dialog));
        for(const auto& pair:{qMakePair(QString("marketMetricsGapAlert"),tr("테더 환산 가격차")),qMakePair(QString("marketMetricsFundingAlert"),tr("펀딩 연환산")),qMakePair(QString("marketMetricsCoinbaseGapAlert"),tr("코인베이스 가격차")),qMakePair(QString("marketMetricsOpenInterestAlert"),tr("미결제약정 흐름"))}){
            auto* original=marketMetricsView_?marketMetricsView_->findChild<QCheckBox*>(pair.first):nullptr;
            if(!original)continue;
            auto* box=mirrorCheck(original,qPrintable("option"+pair.first));box->setText(pair.second);page->addWidget(box);
        }
        if(!marketMetricsView_)page->addWidget(note(tr("시장 지표를 준비하고 있어요."),dialog));
        page->addStretch();
    }
    const auto sync = [mirrored] {
        for (const auto& [source, item] : mirrored) { item->setEnabled(source->isEnabled()); item->setText(source->text()); }
    };
    sync();
    connect(&vault_, &Vault::stateChanged, dialog, sync);
    connect(&vault_, &Vault::accountsChanged, dialog, sync);
    navLayout->addStretch();
    connect(navGroup, &QButtonGroup::idToggled, stack, [this,stack,navGroup](int index, bool selected) {
        if(!selected)return;stack->setCurrentIndex(index);
        if(auto* choice=navGroup->button(index)){optionsPage_=choice->objectName();optionsPage_.chop(3);}
    });
    if(auto* choice=nav->findChild<QPushButton*>(requested+"Tab"))choice->setChecked(true);
    else navGroup->button(0)->setChecked(true);
    // QButtonGroup keeps Up/Down selection native. Home/End stay local to this
    // navigation so editable fields on the page keep their usual key bindings.
    for (const auto& endpoint : {qMakePair(Qt::Key_Home, 0), qMakePair(Qt::Key_End, stack->count()-1)}) {
        auto* action = new QAction(nav);
        action->setShortcut(QKeySequence(endpoint.first));
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        nav->addAction(action);
        connect(action, &QAction::triggered, nav, [navGroup, index = endpoint.second] {
            if (auto* choice = navGroup->button(index)) { choice->click(); choice->setFocus(); }
        });
    }
    for (auto* control : dialog->findChildren<QPushButton*>()) control->setMinimumSize(44, 44);
    for (auto* control : dialog->findChildren<QCheckBox*>()) {
        control->setMinimumSize(44, 44);
        control->setProperty("fullRowTarget", true);
    }
    for (auto* control : dialog->findChildren<QComboBox*>()) {
        // LocalComboBox owns a themed stylesheet. Its content minimum otherwise
        // replaces QWidget's minimum again when a parent/theme is repolished.
        // 28 content + 2x7 padding + 2x1 border = a 44 px settings-only target.
        const auto source = control->property("keefiThemeTemplate").toString();
        setThemeStyle(control, source + QStringLiteral("\nQComboBox { min-height: 28px; }"));
        control->setMinimumHeight(44);
    }
    dialog->setStyleSheet(styleSheet());
    dialog->show();
    // Context navigation selects a page without a mouse press. Start keyboard
    // navigation on that page, rather than leaving the first tab focused.
    if(auto* choice=navGroup->checkedButton())choice->setFocus(Qt::OtherFocusReason);
}


} // namespace keefi
