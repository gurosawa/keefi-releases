#include "keefi/LocalWindow.h"
#include "keefi/LocalWindowHelpers_p.h"
#include "keefi/PlainToolTip.h"
#include "keefi/ProductIdentity.h"
#include "keefi/UsabilityPresentation.h"
#include "keefi/AccountDialog.h"
#include "keefi/AccountModel.h"
#include "keefi/ConnectionPresentation.h"
#include "keefi/CompactControls.h"
#include "keefi/Vault.h"
#include "keefi/VaultDialog.h"
#include "keefi/VaultGate.h"
#include <QFileInfo>
#include "keefi/PortfolioController.h"
#include "keefi/AccountReadAccess.h"
#include "keefi/AssetListFilter.h"
#include "keefi/PortfolioDetail.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/KeefiFonts.h"
#include <QCheckBox>
#include <QDialog>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTimer>
#include <QStackedWidget>
#include <QTableWidget>
#include <QDateTime>
#include "keefi/PortfolioPresentation.h"
#include "keefi/PublicKrwPrices.h"
#include <QStyle>
#include <utility>
#include <functional>
#include <memory>
#include "keefi/StockChartSource.h"
#include "keefi/KiwoomMarketStream.h"
#include "keefi/BrokerTradingController.h"
#include <QAbstractButton>
#include <QComboBox>
#include <QList>
#include <QModelIndex>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>

namespace keefi {
using local_window_detail::textLabel;
using local_window_detail::displayChoice;
using local_window_detail::showTransientFeedback;

int LocalWindow::refreshConnectionStatus() {
    refreshAccountList();
    if (!connectionStatus_ || !portfolio_) return 0;
    auto* pages = findChild<QTabWidget*>("portfolioPages");
    const bool readable = securityReady_ && accountReadsAvailable(vault_);
    const auto states = readable ? portfolio_->connections() : QList<PortfolioConnection>{};
    const QStringList problemStates{"failed", "authentication_required", "account_required", "stopped"};
    int problems = 0;
    const int rows = states.size() + (readable && !states.isEmpty() ? 1 : 0);
    if (connectionStatus_->rowCount() != rows) connectionStatus_->setRowCount(rows);
    const auto setText = [this](int row, int column, const QString& text) {
        auto* cell = connectionStatus_->item(row, column);
        if (!cell) { cell = new QTableWidgetItem; connectionStatus_->setItem(row, column, cell); }
        if (cell->text() != text) { cell->setText(text); cell->setToolTip(plainToolTip(text)); }
    };
    const auto actionButton = [this](int row) {
        auto* action = qobject_cast<QPushButton*>(connectionStatus_->cellWidget(row, 4));
        if (!action) {
            action = new QPushButton(connectionStatus_);
            action->setObjectName("connectionAction");
            applyCompactButton(action);
            connectionStatus_->setCellWidget(row, 4, action);
            connect(action, &QPushButton::clicked, this, [this, action] {
                if (!securityReady_ || !accountReadsAvailable(vault_)) return;
                const auto kind = action->property("actionKind").toString(), id = action->property("connectionId").toString();
                if (kind == "reconnect") feedback_->setText(portfolio_->reconnectConnection(id) ? tr("사용자 요청으로 조회 연결을 복구합니다.") : tr("현재 요청·제한 대기가 끝난 뒤 다시 연결해 주세요."));
                else if (kind == "refresh") portfolio_->refreshConnection(id);
                else if (kind == "prices") portfolio_->refreshPrices();
                refreshPortfolio();
            });
        }
        return action;
    };
    for (int row = 0; row < states.size(); ++row) {
        const auto& c = states[row];
        problems += !c.shared && problemStates.contains(c.state);
        setText(row, 0, c.account.alias + " · " + displayChoice(c.account.market));
        setText(row, 1, c.shared ? tr("공유 연결") : connectionStateName(c));
        setText(row, 2, c.receivedAtMs > 0 ? QDateTime::fromMSecsSinceEpoch(c.receivedAtMs).toString("MM-dd HH:mm:ss") : tr("없음"));
        setText(row, 3, c.storageStatus.isEmpty() ? tr("—") : c.storageStatus);
        auto* action = actionButton(row);
        QString kind, label; bool enabled = false;
        if (c.shared) label = tr("공유 연결");
        else if (c.active) label = c.receivedAtMs>0?tr("다시 조회"):tr("첫 조회 중");
        else if (c.state == "manual") label = tr("거래 화면 사용 중");
        else if (c.state == "unsupported") label = tr("지원 안 함");
        else if (portfolio_->canReconnectConnection(c.account.id)) { kind = "reconnect"; label = c.account.market == "domestic_equity" ? tr("다시 인증") : tr("다시 연결"); enabled = true; }
        else { kind = "refresh"; label = tr("다시 조회"); enabled = true; }
        action->setText(label); action->setEnabled(enabled);
        action->setProperty("actionKind", kind); action->setProperty("connectionId", c.account.id);
        action->setAccessibleName(tr("%1 · %2").arg(c.account.alias, label));
    }
    if (rows > states.size()) {
        const int row = states.size();
        setText(row, 0, tr("원화 가격 · 업비트 공개"));
        const auto pricing = portfolio_->pricingDiagnosticStatus();
        // PublicKrwPrices supplies a sanitized status, never a raw response
        // body. Retained quotes do not make the latest failed request succeed.
        const auto pricingLine=pricing.section('\n',0,0);
        const auto priceState=portfolio_->pricingBusy()
            ? (portfolio_->quotes().isEmpty()?tr("첫 조회 중"):tr("다시 조회 중"))
            : pricingLine.isEmpty()?tr("조회 전"):pricingLine;
        setText(row, 1, priceState);
        connectionStatus_->item(row,1)->setToolTip(pricing);
        setText(row, 2, tr("—")); setText(row, 3, tr("—"));
        auto* action = actionButton(row);
        action->setText(tr("보유 자산 가격 다시 조회")); action->setEnabled(!portfolio_->pricingBusy() && !portfolio_->snapshots().isEmpty());
        action->setProperty("actionKind", "prices"); action->setProperty("connectionId", QString{});
        action->setAccessibleName(tr("보유 자산 전체의 공개 원화 가격 다시 조회"));
    }

    if (pages) pages->setTabText(pages->indexOf(connectionsPage_), tr("계좌·연결"));
    // Per-market rows carry actionable state. Avoid repeating the same error in chrome.
    problemBanner_->hide();
    problemBanner_->setText({});
    syncHeaderTabs();
    refreshTrust();
    return problems;
}

void LocalWindow::setSecurityStatus(bool ready, const QString& statusCode) {
    securityReady_ = ready;
    securityCode_ = statusCode;
    if (!ready)
        lockAndClear();
    if(portfolio_)portfolio_->setAutomaticRefresh(ready);
    refreshState();
}

void LocalWindow::setKeepAwakeState(bool enabled) {
    const QSignalBlocker blocker(keepAwake_);
    keepAwake_->setChecked(enabled);
}

void LocalWindow::refreshState() {
    const bool exists = vault_.exists();
    const bool unlocked = vault_.isUnlocked();
    if(vaultGate_){vaultGate_->refreshMetadata(exists,QFileInfo(vault_.filePath()).lastModified());vaultGate_->setSecurityReady(securityReady_);}
    vaultStatus_->setText(unlocked ? tr("금고 · 잠금 해제됨")
                         : exists ? tr("금고 · 잠김") : tr("금고 · 아직 만들지 않았습니다"));
    platformStatus_->setText(securityReady_
        ? tr("이 PC의 잠금 감지 준비됨 · 금융 인증정보는 선택한 거래소의 공식 API에만 사용합니다")
        : tr("PC 잠금 보호를 준비하지 못해 인증정보 입력과 금고 해제를 사용할 수 없습니다. %1").arg(securityCode_));
    platformStatus_->setVisible(!securityReady_);
    createVault_->setVisible(!exists);
    createVault_->setEnabled(securityReady_ && !exists);
    unlockVault_->setVisible(exists && !unlocked);
    unlockVault_->setEnabled(securityReady_ && exists && !unlocked);
    lockVault_->setEnabled(unlocked || accounts_.rowCount() > 0 || !sensitiveDialog_.isNull());
    lockVault_->setVisible(lockVault_->isEnabled());
    for (auto* item : {createVault_, unlockVault_, lockVault_}) item->setToolTip(vaultStatus_->text());
    changePassword_->setEnabled(securityReady_ && unlocked);
    findChild<QPushButton*>("openRecordTransfer")->setEnabled(securityReady_ && unlocked);
    addAccount_->setEnabled(securityReady_);
    keepAwake_->setEnabled(securityReady_);
    refreshAccountList();
    emptyState_->setText(exists && !unlocked
        ? tr("금고를 해제하면 저장한 계좌가 표시됩니다.")
        : tr("아직 연결한 계좌가 없습니다. ‘계좌 연결’에서 사용할 서비스를 선택하세요."));
    refreshSelection();
}

void LocalWindow::refreshAccountList() {
    if (!connectionListModel_) return;
    const auto selectedId = selectedConnectionId_;
    const QSignalBlocker block(accountList_->selectionModel());
    const bool readable = securityReady_ && vault_.isUnlocked();
    connectionListModel_->refresh(readable ? accounts_.accounts() : QList<AccountRecord>{},
        readable && portfolio_ ? portfolio_->connections() : QList<PortfolioConnection>{});
    int row = connectionListModel_->rowForAccount(selectedId);
    if (row < 0 && connectionListModel_->rowCount()) row = 0;
    accountList_->setCurrentIndex(row < 0 ? QModelIndex{} : connectionListModel_->index(row));
    if (row < 0) { selectedConnectionId_.clear(); connectionDetailShown_ = false; }
}

AccountRecord LocalWindow::selectedAccount() const {
    const auto group = connectionListModel_->groupAt(accountList_->currentIndex().row());
    for (const auto& account : group.accounts) if (account.id == selectedConnectionId_) return account;
    return group.accounts.isEmpty() ? AccountRecord{} : group.accounts.first();
}

void LocalWindow::selectConnection(const QString& id) {
    const int row = connectionListModel_->rowForAccount(id);
    if (row < 0) return;
    const QSignalBlocker block(accountList_->selectionModel());
    accountList_->setCurrentIndex(connectionListModel_->index(row));
    selectedConnectionId_ = id;
    refreshSelection();
}

void LocalWindow::showConnectionAssets(const QString& id) {
    if (!portfolio_ || !portfolioAccount_ || !securityReady_ || !accountReadsAvailable(vault_) || id.isEmpty()) return;
    const auto canonical = portfolio_->canonicalConnection(id);
    // A single connection can finish before the initial portfolio valuation or its queued UI refresh.
    syncPortfolioAccountFilter(portfolio_->connections());
    const int index = portfolioAccount_->findData(canonical);
    if (index < 0) return;
    if (portfolioClass_) portfolioClass_->setCurrentIndex(0);
    portfolioAccount_->setCurrentIndex(index);
    if (portfolioFilter_) if (auto* search = portfolioFilter_->findChild<QLineEdit*>()) search->clear();
    if (portfolioListDetail_) portfolioListDetail_->showList();
    if (auto* pages = findChild<QTabWidget*>("portfolioPages")) pages->setCurrentWidget(findChild<QWidget*>("portfolioHome"));
    syncPortfolioLayout();
}

void LocalWindow::syncConnectionLayout() {
    if (!accountDetail_) return;
    const bool narrow = width() < 1100;
    if (!narrow) connectionDetailShown_ = false;
    const bool selected = !selectedAccount().id.isEmpty();
    const bool detailOnly = narrow && connectionDetailShown_ && selected;
    accountList_->setMinimumWidth(narrow ? 240 : 360);
    accountList_->setMaximumWidth(narrow ? QWIDGETSIZE_MAX : 360);
    accountList_->setVisible(selected && !detailOnly);
    accountDetail_->setVisible(selected && (!narrow || detailOnly));
    accountBack_->setVisible(detailOnly);
}

void LocalWindow::refreshSelection() {
    const auto account = selectedAccount();
    const bool selected = !account.id.isEmpty();
    const bool suspended = AccountModel::isProviderSuspended(account.provider);
    selectedConnectionId_ = account.id;
    emptyState_->setVisible(!selected);
    for (auto* widget : {static_cast<QWidget*>(accountTitle_),
        static_cast<QWidget*>(accountEnvironment_), accountSteps_,
        static_cast<QWidget*>(accountReadState_), static_cast<QWidget*>(accountReadTime_),
        static_cast<QWidget*>(findChild<QPushButton*>("accountMoreToggle"))})
        widget->setVisible(selected);
    replaceAccount_->setEnabled(securityReady_ && selected && !suspended);
    removeAccount_->setEnabled(securityReady_ && selected);
    replaceAccount_->setVisible(selected && !suspended); removeAccount_->setVisible(selected);

    auto* reconnect = findChild<QPushButton*>("reconnectReadAccount");
    auto* holdings = findChild<QPushButton*>("connectionAssets");
    holdings->setVisible(selected && !suspended);
    if (!selected) {
        accountMarket_->hide();
        accountActionScope_->hide();
        reconnect->hide(); reconnect->setEnabled(false);
        refreshSelected_->hide(); holdings->setEnabled(false);
        accountIssue_->hide(); accountIssue_->clear();
        accountProviderReconciliation_->clear();
        const QSignalBlocker block(accountMarket_); accountMarket_->clear();
        accountTitle_->clear(); accountTitle_->setToolTip({}); accountEnvironment_->clear();
        accountReadState_->clear(); accountReadTime_->clear(); accountReadTime_->setToolTip({});
        accountActionScope_->clear();
        accountDetails_->clear(); accountMore_->hide();
        findChild<QPushButton*>("accountMoreToggle")->setChecked(false);
        findChild<QLabel*>("balanceState")->clear();
        auto* grid = qobject_cast<QGridLayout*>(accountSteps_->layout());
        while (auto* old = grid->takeAt(0)) { delete old->widget(); delete old; }
        syncConnectionLayout(); return;
    }
    const auto group = connectionListModel_->groupAt(accountList_->currentIndex().row());
    const auto states = portfolio_ ? portfolio_->connections() : QList<PortfolioConnection>{};
    PortfolioConnection current; current.account = account;
    for (const auto& c : states) if (c.account.id == account.id) { current = c; break; }
    const bool grouped = group.accounts.size() > 1;
    const bool needsAttention = connectionNeedsAttention(current);
    const auto* provider = AccountModel::provider(account.provider);
    const auto& alias = group.accounts.first().alias;
    if (accountTitle_->text() != alias) {
        accountTitle_->setText(alias);
        accountTitle_->setToolTip(plainToolTip(alias));
        accountTitle_->setMaximumWidth(qMin(300, accountTitle_->fontMetrics().horizontalAdvance(alias) + 2));
    }
    // New connections are live only; a badge names an older environment.
    const bool liveEnvironment = account.environment == "live" || account.environment == "mainnet";
    accountEnvironment_->setText(liveEnvironment ? QString{} : connectionEnvironmentName(account.environment));
    accountEnvironment_->setVisible(!liveEnvironment);
    {
        const QSignalBlocker block(accountMarket_);
        bool same = accountMarket_->count() == group.accounts.size();
        for (int i = 0; same && i < group.accounts.size(); ++i)
            same = accountMarket_->itemData(i).toString() == group.accounts[i].id;
        if (!same) accountMarket_->clear();
        for (int i = 0; i < group.accounts.size(); ++i) {
            const auto& a = group.accounts[i];
            PortfolioConnection state;
            for (const auto& c : states) if (c.account.id == a.id) { state = c; break; }
            const auto label = connectionMarketName(a.market) + " · " + connectionStateName(state);
            if (!same) accountMarket_->addItem(label, a.id);
            else if (accountMarket_->itemText(i) != label) accountMarket_->setItemText(i, label);
        }
        accountMarket_->setCurrentIndex(accountMarket_->findData(account.id));
    }
    // Optional rows keep their final visibility throughout a refresh so they cannot shift the detail header.
    accountMarket_->setVisible(grouped);
    const auto wallet = portfolio_ ? phantomWalletReadResult(current, *portfolio_) : std::nullopt;
    const auto stateWidth = accountReadState_->fontMetrics().horizontalAdvance(
        wallet ? tr("Hyperliquid 조회 사용 중") : tr("아직 조회 내역 없음"));
    if (accountReadState_->width() != stateWidth) accountReadState_->setFixedWidth(stateWidth);
    accountReadState_->setText(wallet ? wallet->state : connectionStateName(current));
    const QString tone = current.state == "failed" ? QStringLiteral("error")
        : current.state == "authentication_required" || current.state == "account_required" ? QStringLiteral("warning") : QString{};
    if (accountReadState_->property("tone").toString() != tone) {
        accountReadState_->setProperty("tone", tone);
        accountReadState_->style()->unpolish(accountReadState_);
        accountReadState_->style()->polish(accountReadState_);
    }
    auto readTime = (provider ? provider->label : account.provider) + " · " + connectionMarketName(account.market);
    if (!suspended) readTime += " · " + connectionLastSuccess(current.receivedAtMs);
    accountReadTime_->setText(readTime);
    accountReadTime_->setToolTip(readTime);
    QString reconciliation;
    if (portfolio_ && accountReadsAvailable(vault_)) for (const auto& comparison : portfolio_->providerReconciliations()) {
        if (comparison.official.connectionId != account.id) continue;
        QStringList lines{tr("업체 총액 비교 · %1").arg(providerReconciliationLabel(comparison.state))};
        if (!comparison.official.amount.isEmpty()) lines.append(tr("업체  %1 %2").arg(groupedAmount(comparison.official.amount),comparison.official.currency));
        if (!comparison.keefiAmount.isEmpty()) lines.append(tr("%1  %2 %3").arg(productName(),groupedAmount(comparison.keefiAmount),comparison.official.currency));
        if (!comparison.difference.isEmpty()) lines.append(tr("차이  %1 %2").arg(groupedAmount(comparison.difference),comparison.official.currency));
        if (!comparison.reason.isEmpty()) lines.append(comparison.reason);
        if (!comparison.official.coverage.isEmpty()) lines.append(comparison.official.coverage);
        reconciliation = KeefiFonts::mixedTextHtml(lines.join('\n'), FontRole::Secondary14);
        break;
    }
    accountProviderReconciliation_->setText(reconciliation);
    const auto issue = needsAttention || current.state == "unsupported" ? current.stage : QString{};
    accountIssue_->setText(issue);
    accountIssue_->setVisible(!issue.isEmpty());
    const bool canReconnect = !suspended && securityReady_ && portfolio_ && portfolio_->canReconnectConnection(account.id);
    reconnect->setVisible(canReconnect); reconnect->setEnabled(canReconnect);
    reconnect->setProperty("primary", true);
    reconnect->setText(account.market == "domestic_equity" ? tr("다시 인증") : tr("다시 연결"));
    reconnect->setToolTip(account.market == "domestic_equity"
        ? tr("같은 인증정보를 쓰는 다른 앱의 세션에 영향을 줄 수 있습니다.") : QString{});
    refreshSelected_->setVisible(!canReconnect && !suspended);
    refreshSelected_->setEnabled(!suspended && securityReady_ && portfolio_ && !current.active && !canReconnect &&
        current.state != "unsupported" && current.state != "manual");
    const bool supportedScope = account.environment == "live" || account.environment == "mainnet";
    holdings->setEnabled(!suspended && portfolio_ && current.receivedAtMs > 0 && supportedScope && (!wallet || wallet->hasAssets));
    holdings->setToolTip(supportedScope ? QString{} : tr("지원이 끝난 모의·테스트넷 연결입니다. 계좌·연결에서 삭제하세요."));
    QStringList accountNotes;
    if (wallet && !wallet->detail.isEmpty()) accountNotes.append(wallet->detail);
    if (suspended) accountNotes.append(tr("이 계좌는 조회와 평가 대상에서 제외됩니다. 저장된 연결을 삭제할 수 있어요."));
    else if (grouped) accountNotes.append(tr("선택한 연결: %1 · %2\n키 변경은 현물·선물 모두에 적용됩니다.").arg(account.alias, connectionMarketName(account.market)));
    accountActionScope_->setText(accountNotes.join('\n'));
    accountActionScope_->setVisible(suspended || wallet.has_value() || grouped);
    removeAccount_->setText(grouped ? tr("%1 연결 삭제").arg(connectionMarketName(account.market)) : tr("연결 삭제"));
    replaceAccount_->setText(tr("API 키 변경"));
    if (account.provider == "phantom") replaceAccount_->setText(tr("지갑 주소 변경"));

    const auto id = portfolio_ ? portfolio_->canonicalConnection(account.id) : account.id;
    int fresh = 0, reference = 0, unvalued = 0, excluded = 0;
    for (const auto& g : portfolioView_.groups) for (const auto& h : g.holdings) {
        if (h.connectionId != id || h.presence == AssetPresence::Zero) continue;
        switch (h.state) {
        case PortfolioValueState::Fresh: ++fresh; break;
        case PortfolioValueState::Reference: ++reference; break;
        case PortfolioValueState::Unvalued: ++unvalued; break;
        case PortfolioValueState::NotCounted: ++excluded; break;
        }
    }
    const QString pricing = !supportedScope ? tr("모의·테스트넷 지원 종료 · 조회·평가하지 않음") : current.receivedAtMs <= 0 ? tr("조회 후 확인") :
        tr("최신 %1 · 참고 %2 · 미평가 %3 · 합계 제외 %4").arg(fresh).arg(reference).arg(unvalued).arg(excluded);
    auto* grid = qobject_cast<QGridLayout*>(accountSteps_->layout());
    if (!grid->count()) {
        grid->addWidget(textLabel(tr("저장"), accountSteps_), 0, 0);
        grid->addWidget(textLabel(tr("이 PC의 암호화 금고"), accountSteps_), 0, 1);
        grid->addWidget(textLabel(tr("평가 항목"), accountSteps_), 1, 0);
        grid->addWidget(textLabel({}, accountSteps_, "connectionValuationStatus"), 1, 1);
    }
    findChild<QLabel*>("connectionValuationStatus")->setText(pricing);
    QStringList diagnostics{account.alias + " · " + connectionMarketName(account.market)};
    if (!current.balanceStreamStatus.isEmpty()) diagnostics.append(current.balanceStreamStatus);
    if (!current.storageStatus.isEmpty()) diagnostics.append(current.storageStatus);
    if (account.provider == "okx") diagnostics.append(tr("등록 지역: %1").arg(account.region.isEmpty() ? tr("확인 필요") : account.region));
    accountDetails_->setText(KeefiFonts::mixedTextHtml(diagnostics.join('\n'),FontRole::Secondary14)); accountDetails_->show();
    QStringList coverage;
    if (portfolio_) for (const auto& snapshot : portfolio_->snapshots())
        if (snapshot.connectionId == id) coverage.append(snapshot.coverage);
    findChild<QLabel*>("balanceState")->setText(coverage.join('\n'));
    auto* more = findChild<QPushButton*>("accountMoreToggle");
    if (suspended) more->setChecked(false);
    more->setVisible(!suspended);
    accountMore_->setVisible(!suspended && more->isChecked());
    syncConnectionLayout();
}

void LocalWindow::closeSensitiveDialog() {
    while (sensitiveDialog_) {
        const auto dialog = sensitiveDialog_;
        dialog->reject();
        if (sensitiveDialog_ == dialog) sensitiveDialog_.clear();
    }
}

void LocalWindow::lockAndClear() {
    portfolioAssetsDirty_=false;
    if(vaultGate_)vaultGate_->resetPresentation();
    clearExplorePrivate();
    chartPrivateBlocked_=true;
    clearHoldingMarketSession();
    revokeKiwoomMarketAuthorization();
    revokeStockChartAuthorization();
    clearChartAlertPrivate();
    if(chartWorkspace_)chartWorkspace_->clearPrivate();
    ++exportGeneration_;
    if (exportDialog_) exportDialog_->reject();
    closeSensitiveDialog();

    vault_.lock();
    setKeepAwakeState(false);
    emit keepAwakeRequested(false);
    feedback_->clear();
    refreshState();
}

void LocalWindow::setDeferredWorkbenchInitializer(std::function<void()> initializer) {
    deferredWorkbenchInitializer_=std::move(initializer);
}

void LocalWindow::setBackupRestoreHandler(std::function<BackupRestoreOutcome(const QString&, const QString&, QString*)> handler) {
    backupRestoreHandler_=std::move(handler);
}

void LocalWindow::initializeWorkbenchOnce() {
    // Consume before invoking: source/view signals may synchronously re-enter.
    auto initialize=std::move(deferredWorkbenchInitializer_);
    deferredWorkbenchInitializer_={};
    if(initialize)initialize();
}

void LocalWindow::showVaultGate() {
    if(!startupStack_){
        workbenchRoot_=takeCentralWidget();startupStack_=new QStackedWidget(this);
        startupStack_->setObjectName("startupPages");setCentralWidget(startupStack_);
        startupStack_->addWidget(workbenchRoot_);
        vaultGate_=new VaultGate(vault_.exists(),QFileInfo(vault_.filePath()).lastModified(),startupStack_);
        startupStack_->addWidget(vaultGate_);
        connect(vaultGate_,&VaultGate::browseRequested,this,[this]{lockAndClear();leaveVaultGate(true);});
        connect(vaultGate_,&VaultGate::submitted,this,[this](int mode,const QString& current,const QString& next,const QString& backupPath){
            emit securityCheckRequested();
            if(!securityReady_){vaultGate_->setOperationError(tr("PC 잠금 보호를 준비하지 못해 금고를 사용할 수 없습니다."));return;}
            QString error;
            bool success=false;
            if(!backupPath.isEmpty()&&backupRestoreHandler_){
                const auto result=backupRestoreHandler_(backupPath,current,&error);
                if(result==BackupRestoreOutcome::PreparedForRestart){
                    lockAndClear();
                    // Return through the password submission stack before a
                    // public modal notice starts another event loop.
                    QTimer::singleShot(0,this,[this]{
                        QMessageBox::information(this,tr("백업 복원 준비 완료"),
                            tr("백업을 검증하고 복원할 준비를 마쳤습니다. 앱을 종료합니다. 다시 열면 복원을 마치고 금고 암호를 요청합니다."));
                        requestQuit();
                    });
                    return;
                }
                success=result==BackupRestoreOutcome::Restored;
            }else success=!backupPath.isEmpty()?vault_.restoreBackup(backupPath,current,&error)
                :mode==int(VaultDialog::Mode::Create)?vault_.create(next,&error):vault_.unlock(current,&error);
            refreshState();
            if(success&&securityReady_&&vault_.isUnlocked()){
                prepareUnlockedPortfolio();leaveVaultGate(false);
                showTransientFeedback(feedback_,usabilityText(UsabilityText::VaultReadyNotice));
            }else vaultGate_->setOperationError(error.isEmpty()?tr("금고를 사용할 수 없습니다."):error);
        });
    }
    vaultGate_->refreshMetadata(vault_.exists(),QFileInfo(vault_.filePath()).lastModified());
    vaultGate_->setSecurityReady(securityReady_);startupStack_->setCurrentWidget(vaultGate_);
}

void LocalWindow::leaveVaultGate(bool publicOnly) {
    if(!startupStack_)return;
    initializeWorkbenchOnce();
    vaultGate_->clearSensitiveInputs();
    if(auto* pages=findChild<QTabWidget*>("portfolioPages")){
        if(auto* page=findChild<QWidget*>(publicOnly?"explorePage":"portfolioHome"))pages->setCurrentWidget(page);
    }
    startupStack_->setCurrentWidget(workbenchRoot_);
    syncHeaderTabs();syncPortfolioLayout();syncChartPresentation();
}

void LocalWindow::prepareUnlockedPortfolio() {
    if(!hasSelectedPortfolioReadRegistration())return;
    // Account synchronization starts on the next event turn. Both password
    // presentations hand off to the same loader before any private reads settle.
    resetPortfolioLoadProgress();
    portfolioInitialRefresh_=true;portfolioRestoreCaptured_=false;portfolioRevision_.clear();portfolioInitialStartedMs_=QDateTime::currentMSecsSinceEpoch();
    portfolioWorkbench_->hide();lockedState_->hide();homeBody_->show();
    portfolioLoading_->setText(usabilityText(UsabilityText::PortfolioLoadTitle));portfolioLoading_->show();
    portfolioPendingDetails_->hide();portfolioLoadingPane_->show();
    homeBody_->layout()->activate();portfolioLoadingPane_->repaint();
}

void LocalWindow::openVaultDialog(int mode, std::function<void()> afterSuccess, QDialog* owner) {
    emit securityCheckRequested();
    if (!securityReady_ || (sensitiveDialog_ && sensitiveDialog_ != owner))
        return;
    const QPointer<QDialog> previous = owner;
    auto* dialog = new VaultDialog(static_cast<VaultDialog::Mode>(mode), owner ? static_cast<QWidget*>(owner) : this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    sensitiveDialog_ = dialog;
    connect(dialog, &QDialog::finished, this, [this, dialog, previous] {
        if (sensitiveDialog_ == dialog) sensitiveDialog_ = previous;
    });
    connect(dialog, &VaultDialog::submitted, this, [this, dialog, mode, afterSuccess](const QString& current, const QString& next) {
        emit securityCheckRequested();
        if (!securityReady_) {
            dialog->reject();
            return;
        }
        QString error;
        bool success = false;
        if (mode == int(VaultDialog::Mode::Create)) success = vault_.create(next, &error);
        else if (mode == int(VaultDialog::Mode::Unlock)) success = vault_.unlock(current, &error);
        else success = vault_.changePassword(current, next, &error);
        if (success) {
            if (mode == int(VaultDialog::Mode::Unlock))prepareUnlockedPortfolio();
            dialog->accept();
            showTransientFeedback(feedback_, mode == int(VaultDialog::Mode::ChangePassword) ? tr("금고 암호를 변경했습니다.") : usabilityText(UsabilityText::VaultReadyNotice));
        } else {
            dialog->setError(error);
            feedback_->setText(error);
        }
        refreshState();
        if (success && securityReady_ && afterSuccess) afterSuccess();
    });
#ifdef Q_OS_MACOS
    // QDialog::open turns a parented macOS dialog into a native sheet whose
    // dismissal blocks in AppKit. Keep asynchronous modal input protection
    // without that sheet transition; finished/secret clearing are unchanged.
    dialog->setWindowModality(Qt::ApplicationModal);
    dialog->show();
#else
    dialog->open();
#endif
}

void LocalWindow::openAccountDialog(bool replacement, const QString& chartIdentity, qint64 chartInterval) {
    emit securityCheckRequested();
    if (!securityReady_ || sensitiveDialog_)
        return;
    const auto index = accountList_->currentIndex();
    if (replacement && !index.isValid())
        return;
    const auto account = replacement ? selectedAccount() : AccountRecord{};
    if (replacement && AccountModel::isProviderSuspended(account.provider)) return;
    const bool chartOrigin = !chartIdentity.isEmpty();
    auto* dialog = new AccountDialog(vault_.isUnlocked(), this, chartOrigin);
    dialog->setCredentialSources(vault_.accounts());
    const auto replacementId = std::make_shared<QString>();
    const auto editExisting = [this, dialog, replacementId](const AccountRecord& account) {
        *replacementId = account.id;
        AccountDraft draft;
        draft.provider = account.provider;
        draft.alias = account.alias;
        draft.environment = account.environment;
        draft.market = account.market;
        draft.currency = account.currency;
        draft.region = account.region;
        dialog->setReplacement(draft);
        dialog->setSharedCredentialUsers(vault_.credentialUsers(account.id));
    };
    if (replacement) editExisting(account);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    sensitiveDialog_ = dialog;
    if (chartOrigin) {
        connect(dialog, &AccountDialog::chartReturnRequested, this,
                [this, chartIdentity, chartInterval](const QString& id) {
            returnToBrokerChart(chartIdentity, chartInterval, id);
        });
        connect(dialog, &AccountDialog::existingChartConnectionRequested, this,
                [this, dialog, editExisting](const QString& id) {
            if (!securityReady_ || !vault_.isUnlocked() || !portfolio_) return;
            // A cached registration may be selected before the first portfolio read starts.
            const auto canonical = portfolio_->canonicalConnection(id);
            for (const auto& entry : portfolio_->connections()) {
                const auto& record = entry.account;
                if (record.id != canonical || record.environment != "live" ||
                    (record.provider != "toss" && record.provider != "kiwoom")) continue;
                auto* owner = portfolio_->broker(canonical);
                const bool tossExpired = owner && record.provider == "toss" && chartStockSource_ &&
                    owner == stockChartBroker_ && chartStockSource_->authenticationRequired();
                const bool kiwoomExpired = owner && record.provider == "kiwoom" && chartKiwoomStream_ &&
                    owner == chartKiwoomStream_->broker() && !chartKiwoomStream_->authorizationUsable();
                if (tossExpired || kiwoomExpired || entry.state == "authentication_required") {
                    editExisting(record);
                    return;
                }
                if (!entry.active && entry.state != "complete") portfolio_->refreshConnection(canonical);
                dialog->showConnectionResult(canonical, portfolio_);
                return;
            }
            for (const auto& record : vault_.accounts()) {
                if (record.id == id && record.environment == "live" &&
                    (record.provider == "toss" || record.provider == "kiwoom")) {
                    dialog->showConnectionResult(id, portfolio_);
                    return;
                }
            }
        });
    }
    connect(dialog,&AccountDialog::connectionRequested,this,[this](const QString& id){
        if(auto* pages=findChild<QTabWidget*>("portfolioPages"))pages->setCurrentWidget(connectionsPage_);
        selectConnection(id); connectionDetailShown_ = true; syncConnectionLayout();
    });
    connect(dialog, &AccountDialog::assetsRequested, this, &LocalWindow::showConnectionAssets);
    connect(dialog, &AccountDialog::vaultRequested, this, [this, dialog] {
        openVaultDialog(int(vault_.exists() ? VaultDialog::Mode::Unlock : VaultDialog::Mode::Create),
            [dialog] { dialog->setPersistentAvailable(true); }, dialog);
    });
    connect(dialog, &AccountDialog::submitted, this, [this, dialog, replacementId, chartOrigin](const AccountDraft& draft) {
        emit securityCheckRequested();
        if (!securityReady_) {
            dialog->reject();
            return;
        }
        const auto id = *replacementId;
        const bool replacement = !id.isEmpty();
        if (!AccountModel::allowsNewConnection(draft.provider) ||
            (chartOrigin && draft.provider != "toss" && draft.provider != "kiwoom")) {
            dialog->setError(tr("이 서비스는 연결할 수 없습니다."));
            return;
        }
        QString error;
        QSet<QString> previousIds;
        if (!replacement) for (const auto& a : vault_.accounts()) previousIds.insert(a.id);
        bool success;
        if (replacement) {
            success = draft.provider == "okx"
                ? vault_.replaceCredentials(id, draft.credentials, draft.region, &error)
                : vault_.replaceCredentials(id, draft.credentials, &error);
        } else {
            AccountRecord record;
            record.provider = draft.provider;
            record.alias = draft.alias;
            record.environment = draft.environment;
            record.market = draft.market;
            record.currency = draft.currency;
            record.region = draft.region;
            record.storageMode = StorageMode::EncryptedVault;
            success = draft.sourceAccountId.isEmpty()
                ? vault_.addAccount(record, draft.credentials, record.storageMode, &error)
                : vault_.addAccountUsingCredentials(record, draft.sourceAccountId, &error);
        }
        if (success) {
            QString savedId = id;
            if (!replacement) {
                for (const auto& a : vault_.accounts()) if (!previousIds.contains(a.id)) { savedId = a.id; break; }
            }
            if ((!replacement || chartOrigin) && portfolio_ && !savedId.isEmpty())
                dialog->showConnectionResult(savedId, portfolio_);
            else dialog->accept();
            feedback_->setText(tr("연결 정보를 저장했습니다."));
        } else {
            dialog->setError(error);
            feedback_->setText(error);
        }
        refreshState();
    });
    dialog->open();
}

void LocalWindow::removeSelectedAccount() {
    const auto index = accountList_->currentIndex();
    if (!securityReady_ || !index.isValid() || sensitiveDialog_)
        return;
    const auto account = selectedAccount();
    const auto users = vault_.credentialUsers(account.id);
    QString deletion = tr("‘%1 · %2’ 연결을 이 앱에서 삭제할까요? ").arg(account.alias, connectionMarketName(account.market));
    if (users.size() > 1) {
        QStringList retained;
        for (const auto& user : users) if (user.id != account.id) retained.append(tr("%1 · %2").arg(user.alias, displayChoice(user.market)));
        deletion += tr("공유 키와 다음 연결은 유지합니다: %1. ").arg(retained.join(" / "));
    } else deletion += tr("이 연결에만 쓰는 인증정보도 삭제합니다. ");
    deletion += tr("금융기관의 계좌·API 키·주문과 미확정 주문 기록은 변경하지 않습니다.");
    auto* confirmation = new QMessageBox(QMessageBox::Question, tr("연결 삭제"),
        deletion,
        QMessageBox::NoButton, this);
    confirmation->setObjectName("removeAccountConfirmation");
    confirmation->setTextFormat(Qt::PlainText);
    auto* remove = confirmation->addButton(tr("삭제"), QMessageBox::AcceptRole);
    remove->setObjectName("confirmRemoveAccount");
    auto* cancel = confirmation->addButton(tr("취소"), QMessageBox::RejectRole);
    confirmation->setDefaultButton(cancel);
    confirmation->setAttribute(Qt::WA_DeleteOnClose);
    sensitiveDialog_ = confirmation;
    connect(confirmation, &QMessageBox::buttonClicked, this, [this, remove, id = account.id](QAbstractButton* pressed) {
        if (pressed != remove || !securityReady_)
            return;
        QString error;
        feedback_->setText(vault_.removeAccount(id, &error) ? tr("이 앱의 연결을 삭제했습니다. 다른 연결이 사용하는 공유 키와 미확정 주문 기록은 유지합니다.") : error);
        refreshState();
    });
    confirmation->open();
}

void LocalWindow::confirmEraseVault() {
    if (!securityReady_ || !vault_.exists()) return;
    closeSensitiveDialog();
    auto* confirmation = new QMessageBox(QMessageBox::Warning, tr("금고 지우기"), tr("정말 지우겠습니까?"), QMessageBox::NoButton, this);
    confirmation->setObjectName("eraseVaultConfirmation");
    confirmation->setTextFormat(Qt::PlainText);
    confirmation->setInformativeText(tr("이 PC에 저장한 계좌 연결(API 키)과 암호화된 자산·스왑·가져온 기록, 시세 기록이 모두 지워지고 되돌릴 수 없어요. "
                                        "거래소·증권사의 실제 계좌와 API 키는 바뀌지 않아요. 지운 뒤 새 금고를 만들 수 있어요."));
    auto* erase = confirmation->addButton(tr("지우기"), QMessageBox::DestructiveRole);
    erase->setObjectName("confirmEraseVault");
    auto* cancel = confirmation->addButton(tr("취소"), QMessageBox::RejectRole);
    cancel->setObjectName("cancelEraseVault");
    confirmation->setDefaultButton(cancel);
    confirmation->setEscapeButton(cancel);
    confirmation->setAttribute(Qt::WA_DeleteOnClose);
    sensitiveDialog_ = confirmation;
    connect(confirmation, &QMessageBox::buttonClicked, this, [this, erase](QAbstractButton* pressed) {
        if (pressed != erase || !securityReady_) return;
        lockAndClear();
        QString error;
        feedback_->setText(vault_.erase(&error) ? tr("금고를 지웠어요. 새 금고를 만들 수 있어요.") : error);
        refreshState();
    });
    confirmation->open();
}


} // namespace keefi
