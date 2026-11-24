#include "keefi/LocalWindow.h"
#include "keefi/LocalWindowHelpers_p.h"
#include "keefi/ProductIdentity.h"
#include "keefi/ProfileSettings.h"
#include "keefi/MarketFlowHistory.h"
#include "keefi/LocalCheckBox.h"
#include "keefi/AccountModel.h"
#include "keefi/ConnectionPresentation.h"
#include "keefi/CompactControls.h"
#include "keefi/Vault.h"
#include "keefi/VaultDialog.h"
#include "keefi/VaultGate.h"
#include "keefi/ReleaseUpdater.h"
#include <QFileInfo>
#include "keefi/AssetsWindow.h"
#include "keefi/RecordTransferDialog.h"
#include "keefi/SwapJournal.h"
#include "keefi/PortfolioController.h"
#include "keefi/ValuationPreferences.h"
#include "keefi/SegmentedComboBox.h"
#include "keefi/PortfolioDetail.h"
#include "keefi/PortfolioAllocation.h"
#include "keefi/PortfolioDayView.h"
#include "keefi/MarketMetricsView.h"
#include "keefi/TodayBtcPremium.h"
#include "keefi/WorldMarkets.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/ExploreDashboard.h"
#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/BackgroundTray.h"
#include "keefi/ChartAlertEngine.h"
#include "keefi/ChartAlertFeed.h"
#include "keefi/CoinFlowSource.h"
#include "keefi/MarketBreadthMonitor.h"
#include "keefi/RapidMoveMonitor.h"
#include "keefi/ChartStudyFeed.h"
#include "keefi/RankingFuturesFeed.h"
#include "keefi/RankingStudyRunner.h"
#include "keefi/ChartAlertsPanel.h"
#include "keefi/StockChartSource.h"
#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/KiwoomMarketStream.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include <QAction>
#include <QGuiApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDialog>
#include <QDesktopServices>
#include <QUrl>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QListView>
#include <QSettings>
#include "keefi/MovementColorPolicy.h"
#include <QMenu>
#include <QMessageBox>
#include "keefi/ResponseJournal.h"
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QHideEvent>
#include <QVBoxLayout>
#include <QTabWidget>
#include <QTimer>
#include "keefi/MarketCandles.h"
#include "keefi/MarketOverview.h"
#include "keefi/OfficialNotices.h"
#include "keefi/PortfolioPresentation.h"
#include "keefi/ValuationBundle.h"
#include <QScreen>
#include <QStyle>
#include <utility>
#include <functional>
#include <memory>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QByteArray>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QMainWindow>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QSize>
#include <QScrollArea>
#include <QSizePolicy>
#include <QString>
#include <QWidget>

namespace keefi {
using local_window_detail::textLabel;
using local_window_detail::ledgerTextLabel;
using local_window_detail::button;

LocalWindow::LocalWindow(Vault& vault, AccountModel& accounts, QWidget* parent, BackgroundTray* trayBackend)
    : QMainWindow(parent), vault_(vault), accounts_(accounts) {
    setObjectName("keefiLocalWindow");
    KeefiFonts::registerBundledFonts();
    KeefiTheme::instance()->setSettings(new ProfileSettings(this));
    movementColors_=new MovementColorPolicy(this);movementPublicState_=movementColors_->publicState();
    restoreMoneyCurrency();
    connect(movementColors_,&MovementColorPolicy::changed,this,[this]{
        const auto state=movementColors_->publicState();
        if(!restoringMovementColors_&&chartSettings_&&state!=movementPublicState_){chartSettings_->setValue("display/movementColors/v1",state);movementPublicState_=state;}
        refreshMovementPresentation();
    });
    const bool development = isDevelopmentApplicationName(QCoreApplication::applicationName());
    const QString displayName = development ? devApplicationName() : productName();
    setWindowTitle(tr("%1 · 자산과 계좌").arg(displayName));
    setMinimumSize(720, 640);
    {
        const auto* screen = QGuiApplication::primaryScreen();
        const QSize available = (screen ? screen->availableGeometry().size() : QSize(1840, 1165)) - QSize(40, 40);
        const auto bounds = available.expandedTo(minimumSize());
        const auto saved = ProfileSettings().value("display/windowSize/v1").toSize();
        if (saved.isValid()) resize(saved.boundedTo(bounds).expandedTo(minimumSize()));
        else {
            const double scale = qMin(1.0, qMin(available.width() / 1800.0, available.height() / 1125.0));
            resize(QSize(int(1800 * scale), int(1125 * scale)).expandedTo(minimumSize()));
        }
    }
    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(18, 14, 18, 14);
    layout->setSpacing(8);
    setWindowIcon(crosslineIcon());
    // Header: name · underlined page tabs · source state and vault controls.
    auto* heading = new QHBoxLayout;
    heading->setObjectName("workbenchHeading");
    heading->setSpacing(10);
    // Equal-stretch side boxes keep the page tabs centred whatever sits beside them.
    auto* leftBox = new QWidget(central);
    auto* left = new QHBoxLayout(leftBox);
    left->setContentsMargins(0, 0, 0, 0);
    left->setSpacing(10);
    auto* rightBox = new QWidget(central);
    auto* right = new QHBoxLayout(rightBox);
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(8);
    auto* brand = textLabel(displayName, central, "brandLabel");
    brand->setProperty("type", "brand");
    brand->setFont(KeefiFonts::font(FontRole::Logo22));
    brand->setWordWrap(false);
    left->addWidget(brand);
    left->addStretch(1);
    heading->addWidget(leftBox, 1);
    headerTabs_ = new QWidget(central);
    headerTabs_->setObjectName("headerTabs");
    headerTabs_->setAttribute(Qt::WA_StyledBackground, true);
    auto* tabsLayout = new QHBoxLayout(headerTabs_);
    tabsLayout->setContentsMargins(4, 4, 4, 4);
    tabsLayout->setSpacing(4);
    headerTabs_->hide();
    heading->addWidget(headerTabs_);
    heading->addWidget(rightBox, 1);
    right->addStretch(1);
    auto* themeSwitch = new QWidget(central);
    themeSwitch->setObjectName("themeSwitch");
    auto* themeButtons = new QHBoxLayout(themeSwitch);
    themeButtons->setContentsMargins(0, 0, 0, 0); themeButtons->setSpacing(0);
    auto* lightChoice = new QPushButton(tr("흰"), themeSwitch);
    auto* darkChoice = new QPushButton(tr("검"), themeSwitch);
    lightChoice->setObjectName("themeLight"); darkChoice->setObjectName("themeDark");
    for (auto* choice : {lightChoice, darkChoice}) {
        choice->setProperty("themeChoice", true); choice->setCheckable(true); choice->setAutoExclusive(true); themeButtons->addWidget(choice);
    }
    const auto syncTheme = [lightChoice, darkChoice] {
        const QSignalBlocker lightBlock(lightChoice), darkBlock(darkChoice);
        lightChoice->setChecked(!KeefiTheme::instance()->isDark());
        darkChoice->setChecked(KeefiTheme::instance()->isDark());
    };
    connect(lightChoice, &QPushButton::toggled, this, [](bool selected) { if (selected) KeefiTheme::instance()->setMode(ThemeMode::Light); });
    connect(darkChoice, &QPushButton::toggled, this, [](bool selected) { if (selected) KeefiTheme::instance()->setMode(ThemeMode::Dark); });
    // Clicking the already selected system color still pins an explicit mode.
    connect(lightChoice, &QPushButton::clicked, this, [] { KeefiTheme::instance()->setMode(ThemeMode::Light); });
    connect(darkChoice, &QPushButton::clicked, this, [] { KeefiTheme::instance()->setMode(ThemeMode::Dark); });
    connect(KeefiTheme::instance(), &KeefiTheme::changed, themeSwitch, syncTheme); syncTheme();
    right->addWidget(themeSwitch);
    trustPill_ = new QPushButton(central);
    trustPill_->setObjectName("trustPill");
    trustPill_->setProperty("pill", true);
    trustPill_->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed);
    trustPill_->setAccessibleName(tr("숫자가 만들어진 과정 보기"));
    trustPill_->hide();
    right->addWidget(trustPill_);
    connect(trustPill_, &QPushButton::clicked, this, &LocalWindow::showTrustProcess);
    vaultStatus_ = textLabel(QString(), central, "vaultStatus");
    vaultStatus_->setWordWrap(false);
    vaultStatus_->setProperty("type", "muted");
    right->addWidget(vaultStatus_);
    createVault_ = button(tr("금고 만들기"), "createVault", central);
    unlockVault_ = button(tr("잠금 해제"), "unlockVault", central);
    lockVault_ = button(tr("금고 잠금"), "lockVault", central);
    changePassword_ = button(tr("암호 변경"), "changeVaultPassword", central);
    for (auto* item : {createVault_, unlockVault_, lockVault_, changePassword_}) {
        item->setProperty("compact", true);
        right->addWidget(item);
    }
    createVault_->setProperty("primary", true);
    unlockVault_->setProperty("primary", true);
    vaultStatus_->hide();      // The lock buttons already say the state; the text stays for tooltips.
    changePassword_->hide();   // Rare: reached from the menu below.
    auto* optionsButton = new QPushButton(central);
    optionsButton->setObjectName("openOptions");
    optionsButton->setFixedSize(40,40);
    optionsButton->setIconSize(QSize(20,20));
    bindThemePresentation(optionsButton,[optionsButton]{
        QPixmap image(40,40); image.setDevicePixelRatio(2); image.fill(Qt::transparent);
        QPainter painter(&image); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(themeColors().text3,1.5));
        for(int row=0;row<3;++row) {
            const qreal y=4+row*6, knob=row==1?13:7;
            painter.drawLine(QPointF(2,y),QPointF(knob-2,y));
            painter.drawLine(QPointF(knob+2,y),QPointF(18,y));
            painter.drawEllipse(QPointF(knob,y),2,2);
        }
        optionsButton->setIcon(QIcon(image));
    });
    optionsButton->setProperty("compact", true);
    optionsButton->setAccessibleName(tr("옵션"));
    optionsButton->setToolTip(tr("옵션 (⌘,)"));
    connect(optionsButton, &QPushButton::clicked, this, &LocalWindow::openOptions);
    right->addWidget(optionsButton);
    vaultStatus_->setToolTip(tr("계좌 연결 정보는 이 PC의 금고에 암호화 보관하며 잠금 해제로 복원합니다. 잠그면 화면의 개인 정보를 지웁니다."));
    layout->addLayout(heading);
    auto* headerRule = new QFrame(central);
    headerRule->setObjectName("workbenchHeaderRule");
    headerRule->setFixedHeight(1);
    setThemeStyle(headerRule,"background:${rule};border:0;");
    layout->addWidget(headerRule);
    assetsWindow_=new AssetsWindow(vault_,this);
    auto* quit = button(tr("앱 종료"), "quitApplication", central);
    connect(quit, &QPushButton::clicked, this, &LocalWindow::requestQuit);
    auto* assets=button(tr("자산·성과 기록"),"openAssets",central);
    connect(assets,&QPushButton::clicked,this,[this]{assetsWindow_->openRecords();});
    auto* transfer=button(tr("기록 이동"),"openRecordTransfer",central);
    connect(transfer,&QPushButton::clicked,this,[this]{
        if(!securityReady_||!vault_.isUnlocked())return;

        if (!historicalSwaps_) historicalSwaps_ = new SwapJournal(&vault_, QFileInfo(vault_.filePath()).dir().filePath("swaps.keefi"), this);
        if (!recordTransferDialog_) recordTransferDialog_ = new RecordTransferDialog(vault_, portfolio_, historicalSwaps_, this);
        recordTransferDialog_->openRecords();
    });
    auto* stage = textLabel(tr("통합 자산 · 계좌를 연결하면 자동 조회합니다. 기본은 조회전용입니다."), central, "stageBanner");
    stage->setProperty("type", "banner");
    layout->addWidget(stage);
    platformStatus_ = textLabel(QString(), central, "platformSecurityStatus");
    platformStatus_->setProperty("tone", "warning");
    layout->addWidget(platformStatus_);
    connect(createVault_, &QPushButton::clicked, this, [this] { openVaultDialog(int(VaultDialog::Mode::Create)); });
    connect(unlockVault_, &QPushButton::clicked, this, [this] { openVaultDialog(int(VaultDialog::Mode::Unlock)); });
    connect(changePassword_, &QPushButton::clicked, this, [this] { openVaultDialog(int(VaultDialog::Mode::ChangePassword)); });
    connect(lockVault_, &QPushButton::clicked, this, &LocalWindow::lockAndClear);
    accountPanel_=new QWidget(central);auto* accountLayout=new QVBoxLayout(accountPanel_);accountLayout->setContentsMargins(0,0,0,0);
    auto* accountHeading = new QHBoxLayout;
    auto* accountTitle = textLabel(tr("연결 계좌"), central);
    accountTitle->setProperty("type", "section");
    accountHeading->addWidget(accountTitle);
    accountHeading->addStretch();
    const auto addGuide = [this, accountHeading](const QString& provider, const QString& title, const char* name) {
        auto* guide = button(title + QStringLiteral(" ↗"), name, accountPanel_);
        guide->setAccessibleName(tr("%1를 브라우저에서 열기").arg(title));
        accountHeading->addWidget(guide);
        connect(guide, &QPushButton::clicked, this, [this, provider, title] {
            const auto* spec = AccountModel::provider(provider);
            if (!spec || !QDesktopServices::openUrl(QUrl(spec->guideUrl)))
                local_window_detail::showTransientFeedback(feedback_, tr("기본 브라우저에서 %1를 열 수 없어요.").arg(title));
        });
    };
    addGuide("phantom", tr("팬텀 주소 복사 가이드"), "phantomAddressGuide");
    addGuide("bithumb", tr("빗썸 연결 가이드"), "bithumbConnectionGuide");
    addGuide("okx", tr("OKX 연결 가이드"), "okxConnectionGuide");
    addAccount_ = button(tr("계좌 연결"), "addAccount", central);
    addAccount_->setProperty("primary", true);
    accountHeading->addWidget(addAccount_);
    accountLayout->addLayout(accountHeading);
    connect(addAccount_, &QPushButton::clicked, this, [this] { openAccountDialog(false); });
    emptyState_ = textLabel({}, accountPanel_, "emptyAccountState");
    emptyState_->setAlignment(Qt::AlignCenter);
    accountLayout->addWidget(emptyState_, 1);
    auto* accountArea = new QHBoxLayout;
    accountList_ = new QListView(central);
    accountList_->setObjectName("accountList");
    connectionListModel_ = new ConnectionListModel(this);
    accountList_->setModel(connectionListModel_);
    accountList_->setAccessibleName(tr("연결 계좌와 시장별 조회 상태"));
    accountList_->setMinimumWidth(240);
    accountList_->setMaximumWidth(360);
    accountList_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    accountList_->setItemDelegate(new ConnectionRowDelegate(accountList_));
    accountList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    accountList_->setTextElideMode(Qt::ElideRight);
    accountList_->setMouseTracking(true);
    accountArea->addWidget(accountList_);
    auto* detailCard = new QFrame(central);
    accountDetail_ = detailCard;
    detailCard->setObjectName("connectionDetail");
    detailCard->setMinimumWidth(0);
    // The detail reads as part of the page, not a box with empty space below.
    detailCard->setProperty("card", false);
    auto* detailsLayout = new QVBoxLayout(detailCard);
    detailsLayout->setSpacing(10);
    accountBack_ = button(tr("‹ 계좌 목록"), "connectionDetailBack", detailCard);
    detailsLayout->addWidget(accountBack_, 0, Qt::AlignLeft);
    connect(accountBack_, &QPushButton::clicked, this, [this] {
        connectionDetailShown_ = false; syncConnectionLayout(); accountList_->setFocus();
    });
    auto* detailHeader = new QFrame(detailCard);
    detailHeader->setObjectName("connectionDetailHeader");
    detailHeader->setFixedHeight(72);
    setThemeStyle(detailHeader, QStringLiteral("QFrame#connectionDetailHeader { border-bottom: 1px solid ${rule}; }"));
    auto* headerLayout = new QVBoxLayout(detailHeader);
    headerLayout->setContentsMargins(0, 4, 0, 4);
    headerLayout->setSpacing(4);
    detailsLayout->addWidget(detailHeader);

    // A group can share a key; every market keeps its own status and action target.
    auto* identityRow = new QHBoxLayout;
    identityRow->setSpacing(8);
    accountTitle_ = textLabel({}, detailHeader, "accountTitle");
    accountTitle_->setProperty("type", "section");
    accountTitle_->setFont(KeefiFonts::font(FontRole::Title17));
    accountTitle_->setWordWrap(false);
    accountTitle_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    identityRow->addWidget(accountTitle_, 1);
    accountEnvironment_ = textLabel({}, detailHeader, "accountEnvironment");
    accountEnvironment_->setProperty("type", "badge");
    accountEnvironment_->setWordWrap(false);
    identityRow->addWidget(accountEnvironment_, 0, Qt::AlignVCenter);
    auto* accountMarkets = new SegmentedComboBox(detailHeader);
    accountMarkets->setCompact();
    accountMarkets->setFixedHeight(CompactButtonHeight + 4);
    accountMarkets->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    accountMarket_ = accountMarkets;
    accountMarket_->setObjectName("connectionMarketSelector");
    accountMarket_->setAccessibleName(tr("연결 시장 선택"));
    identityRow->addWidget(accountMarket_, 0, Qt::AlignVCenter);
    identityRow->addStretch();
    headerLayout->addLayout(identityRow);
    connect(accountMarket_, &QComboBox::currentIndexChanged, this, [this] {
        selectedConnectionId_ = accountMarket_->currentData().toString(); refreshSelection();
    });

    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(6);
    accountReadState_ = textLabel({}, detailHeader, "connectionReadState");
    accountReadState_->setFont(KeefiFonts::font(FontRole::CaptionStrong12));
    accountReadState_->setWordWrap(false);
    accountReadState_->setFixedSize(accountReadState_->fontMetrics().horizontalAdvance(tr("아직 조회 내역 없음")), 22);
    setThemeStyle(accountReadState_, QStringLiteral(R"(
        QLabel { color: ${text2}; }
        QLabel[tone="warning"] { color: ${accentText}; }
        QLabel[tone="error"] { color: ${danger}; }
    )"));
    statusRow->addWidget(accountReadState_, 0, Qt::AlignVCenter);
    accountReadTime_ = ledgerTextLabel({}, FontRole::Caption12, detailHeader, "connectionReadTime");
    accountReadTime_->setWordWrap(false);
    accountReadTime_->setFixedHeight(22);
    accountReadTime_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setThemeStyle(accountReadTime_, QStringLiteral("QLabel { color: ${text2}; }"));
    statusRow->addWidget(accountReadTime_, 1, Qt::AlignVCenter);
    headerLayout->addLayout(statusRow);

    accountSteps_ = new QWidget(detailCard);
    accountSteps_->setObjectName("accountSteps");
    accountSteps_->setProperty("placeRow", true);
    accountSteps_->setAttribute(Qt::WA_StyledBackground, true);
    auto* stepsLayout = new QGridLayout(accountSteps_);
    stepsLayout->setContentsMargins(14, 10, 14, 10);
    stepsLayout->setVerticalSpacing(8);
    auto* more = new QPushButton(tr("연결 진단 ▾"), detailHeader);
    more->setObjectName("accountMoreToggle");
    more->setCheckable(true);
    more->setProperty("disclose", true);
    // Status messages can gain and lose lines as a read progresses. Let the
    // expanded diagnostic contents scroll without moving the account actions.
    auto* diagnostics = new QScrollArea(detailCard);
    accountMore_ = diagnostics;
    diagnostics->setObjectName("accountMore");
    diagnostics->setWidgetResizable(true);
    diagnostics->setFrameShape(QFrame::NoFrame);
    diagnostics->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    diagnostics->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    diagnostics->setMinimumHeight(4 * CompactButtonHeight);
    auto* diagnosticContents = new QWidget(diagnostics);
    auto* moreLayout = new QVBoxLayout(diagnosticContents);
    moreLayout->setContentsMargins(4, 0, 4, 0);
    accountIssue_ = textLabel({}, accountMore_, "connectionIssue");
    accountIssue_->setProperty("tone", "warning");
    accountDetails_ = textLabel({}, accountMore_, "accountDetails");
    accountDetails_->setFont(KeefiFonts::font(FontRole::Secondary14));accountDetails_->setTextFormat(Qt::AutoText);
    accountDetails_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    accountDetails_->setProperty("type", "muted");
    moreLayout->addWidget(accountSteps_);
    moreLayout->addWidget(accountDetails_);
    moreLayout->addWidget(accountIssue_);
    accountProviderReconciliation_ = textLabel({}, accountMore_, "accountProviderReconciliation");
    accountProviderReconciliation_->setFont(KeefiFonts::font(FontRole::Secondary14));accountProviderReconciliation_->setTextFormat(Qt::AutoText);
    accountProviderReconciliation_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    moreLayout->addWidget(accountProviderReconciliation_);
    accountMore_->hide();
    detailsLayout->addWidget(accountMore_, 1);
    connect(more, &QPushButton::toggled, this, [this, more](bool open) {
        accountMore_->setVisible(open);
        more->setText(open ? tr("연결 진단 접기 ▴") : tr("연결 진단 ▾"));
    });
    auto* balance = textLabel(tr("잔고 · 보유 자산 · 평가손익\n—  아직 조회하지 않았습니다"), detailCard, "balanceState");
    balance->setProperty("type", "muted");
    moreLayout->addWidget(balance);
    moreLayout->addStretch();
    diagnostics->setWidget(diagnosticContents);

    auto* reconnect = button(tr("다시 연결"), "reconnectReadAccount", detailHeader);
    reconnect->setEnabled(false);
    refreshSelected_ = button(tr("이 계좌 다시 조회"), "refreshSelectedConnection", detailHeader);
    connect(refreshSelected_, &QPushButton::clicked, this, [this] {
        if (!securityReady_ || !portfolio_ || !accountList_->currentIndex().isValid()) return;
        portfolio_->refreshConnection(selectedAccount().id);
        refreshSelection();
    });

    connect(reconnect, &QPushButton::clicked, this, [this] {
        if(!securityReady_||!portfolio_||!accountList_->currentIndex().isValid())return;
        const auto id=selectedAccount().id;
        feedback_->setText(portfolio_->reconnectConnection(id) ? QString{} : tr("현재 요청·제한 대기가 끝난 뒤 다시 연결해 주세요."));
        refreshSelection();
    });
    auto* holdings = button(tr("보유 자산 보기"), "connectionAssets", detailHeader);
    holdings->setProperty("primary", true);
    // Read and recovery are mutually exclusive actions on this connection.
    // Sharing a grid cell keeps the other controls anchored on error and retry.
    auto* readAction = new QGridLayout;
    for (auto* action : {refreshSelected_, reconnect}) {
        QSizePolicy size(QSizePolicy::Preferred, QSizePolicy::Fixed);
        size.setRetainSizeWhenHidden(true);
        action->setSizePolicy(size);
        readAction->addWidget(action, 0, 0);
    }
    statusRow->addLayout(readAction);
    for (auto* action : {more, holdings}) {
        action->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        statusRow->addWidget(action, 0, Qt::AlignVCenter);
    }
    const auto expandedWidth = more->fontMetrics().horizontalAdvance(tr("연결 진단 접기 ▴"));
    const auto collapsedWidth = more->fontMetrics().horizontalAdvance(more->text());
    more->setFixedWidth(qMax(expandedWidth, collapsedWidth) + 24);
    connect(holdings, &QPushButton::clicked, this, [this] { showConnectionAssets(selectedAccount().id); });
    accountActionScope_ = textLabel({}, detailCard, "connectionActionScope");
    accountActionScope_->setProperty("type", "muted");
    detailsLayout->addWidget(accountActionScope_);
    auto* accountActions = new QHBoxLayout;
    replaceAccount_ = button(tr("인증정보 교체"), "replaceCredentials", detailCard);
    removeAccount_ = button(tr("연결 삭제"), "removeAccount", detailCard);
    removeAccount_->setProperty("tone", "error");
    accountActions->addWidget(replaceAccount_);
    accountActions->addWidget(removeAccount_);
    accountActions->addStretch();
    // Key and removal actions sit under the status they act on.
    detailsLayout->addLayout(accountActions);
    detailsLayout->addStretch();
    accountArea->addWidget(detailCard, 1);
    accountLayout->addLayout(accountArea, 1);
    for (auto* control : accountPanel_->findChildren<QPushButton*>())
        applyCompactButton(control);
    layout->addWidget(accountPanel_,1);
    connect(replaceAccount_, &QPushButton::clicked, this, [this] { openAccountDialog(true); });
    connect(removeAccount_, &QPushButton::clicked, this, &LocalWindow::removeSelectedAccount);
    connect(accountList_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] {
        selectedConnectionId_.clear(); refreshSelection();
    });
    const auto showAccountDetail = [this] { connectionDetailShown_ = true; syncConnectionLayout(); };
    connect(accountList_, &QListView::clicked, this, showAccountDetail);
    connect(accountList_, &QListView::activated, this, showAccountDetail);
    feedback_ = textLabel(QString(), central, "localFeedback");
    layout->addWidget(feedback_);
    auto* options = new QHBoxLayout;
    background_ = new LocalCheckBox(tr("창을 닫아도 백그라운드에서 실행"), central);
    background_->setObjectName("backgroundOption");
    keepAwake_ = new LocalCheckBox(tr("실행 중 PC 자동 절전 방지"), central);
    keepAwake_->setObjectName("keepAwakeOption");
    options->addWidget(background_);
    options->addWidget(keepAwake_);
    keepAwake_->setToolTip(tr("화면 꺼짐과 PC 잠금은 다릅니다. PC를 잠그면 금고도 잠깁니다. 전원이 꺼진 PC에서는 실행되지 않습니다."));
    options->addStretch();
    for (auto* item : {transfer, assets, quit}) {
        item->setProperty("compact", true);
        options->addWidget(item);
    }
    layout->addLayout(options);
    // App-level settings and actions live in the options window; the buttons stay as
    // the single source of each action's enabled state.
    for (auto* widget : std::initializer_list<QWidget*>{background_, keepAwake_, transfer, assets, quit}) widget->hide();
    auto* optionsShortcut = new QAction(this);
    optionsShortcut->setObjectName("optionsShortcut");
    optionsShortcut->setShortcut(QKeySequence::Preferences);
    connect(optionsShortcut, &QAction::triggered, this, &LocalWindow::openOptions);
    addAction(optionsShortcut);
    auto* quitShortcut = new QAction(this);
    quitShortcut->setShortcut(QKeySequence::Quit);
    connect(quitShortcut, &QAction::triggered, this, &LocalWindow::requestQuit);
    addAction(quitShortcut);
    connect(keepAwake_, &QCheckBox::toggled, this, &LocalWindow::keepAwakeRequested);
    tray_=trayBackend?trayBackend:makeBackgroundTray(crosslineIcon(),displayName,this);
    if (tray_ && tray_->available()) {
        auto* menu = new QMenu(this);trayMenu_=menu;
        auto* show = menu->addAction(tr("%1 열기").arg(displayName));
        auto* mute = menu->addMenu(tr("알림 잠시 끄기"));
        auto* hour = mute->addAction(tr("1시간"));auto* today = mute->addAction(tr("오늘"));
        auto* drip = menu->addAction(tr("드립 끄기"));
        auto* lock = menu->addAction(tr("금고 잠금"));
        menu->addSeparator();
        auto* exit = menu->addAction(tr("앱 종료"));
        connect(show, &QAction::triggered, this, [this] { showNormal(); raise(); activateWindow(); });
        connect(lock, &QAction::triggered, this, &LocalWindow::lockAndClear);
        connect(hour,&QAction::triggered,this,[this]{if(chartAlerts_)chartAlerts_->muteForOneHour();});
        connect(today,&QAction::triggered,this,[this]{if(chartAlerts_)chartAlerts_->muteForToday();});
        connect(drip,&QAction::triggered,this,[this]{if(chartAlerts_)chartAlerts_->setDripEnabled(false);});
        connect(exit, &QAction::triggered, this, &LocalWindow::requestQuit);
        tray_->setMenu(menu);
        connect(tray_, &BackgroundTray::activated, this, [this] {showNormal();raise();activateWindow();});
        tray_->show();
        background_->setChecked(true);
    } else {
        background_->setEnabled(false);
        background_->setToolTip(tr("이 실행 환경에서는 시스템 트레이를 사용할 수 없습니다."));
    }
    connect(background_,&QCheckBox::toggled,this,[this](bool enabled){
        if(chartAlertsPanel_)chartAlertsPanel_->setBackgroundEnabled(enabled);
        if(tray_&&tray_->available()){if(enabled)tray_->show();else tray_->hide();}
    });
    valuationPreferences_ = new ValuationPreferences(vault_, this);
    connect(valuationPreferences_, &ValuationPreferences::changed, this, [this] {
        portfolioRevision_.clear(); portfolioCachedView_ = {}; refreshPortfolio(); refreshDayHistory();
    });
    valuationPreferences_->load();
    connect(&vault_, &Vault::aboutToLock, this, [this] {
        portfolioAssetsDirty_=false;
        if(vaultGate_)vaultGate_->clearSensitiveInputs();
        clearExplorePrivate();
        chartPrivateBlocked_=true;
        clearHoldingMarketSession();
        revokeKiwoomMarketAuthorization();
        revokeStockChartAuthorization();
        refreshStockConnectionPresentation();
        clearChartAlertPrivate();
        // Revoke the display cache and its callbacks before private state clears.
        if(chartCandles_)chartCandles_->clearDisplayCache();
        if(chartWorkspace_)chartWorkspace_->clearPrivate();
        // Recorded responses hold account data; locking ends the recording.
        ResponseJournal::setEnabled(false);
        lastValuation_ = {};
        if (portfolioDayView_) portfolioDayView_->clear(tr("금고를 열면 표시"));
        ++exportGeneration_;
        if (exportDialog_) exportDialog_->reject();
        closeSensitiveDialog();

    });
    connect(&vault_, &Vault::stateChanged, this, [this]{
        chartPrivateBlocked_=!vault_.isUnlocked();refreshState();refreshChartWatchlists();refreshStockConnectionPresentation();
    });
    connect(&vault_, &Vault::accountsChanged, this, &LocalWindow::refreshState);
    connect(&accounts_, &QAbstractItemModel::modelReset, this, &LocalWindow::refreshState);
    applyLocalTheme();
    refreshState();
}

LocalWindow::~LocalWindow() {
    // QWidget hides its children after the window's own members have been
    // destroyed. Hiding the chart synchronously clears its candle selection.
    if (chartWorkspace_) disconnect(chartWorkspace_, nullptr, this, nullptr);
    if (chartCandles_) disconnect(chartCandles_, nullptr, this, nullptr);
    clearAssetShare();
    publicObservationsRefreshing_=true;rankingRefreshing_=true;
    // The owned transport can notify its catalogue while QWidget deletes children,
    // after this window's ranking cards and their member storage are gone.
    if(chartInstruments_)disconnect(chartInstruments_,nullptr,this,nullptr);
    if(rankingStudyRunner_){disconnect(rankingStudyRunner_,nullptr,this,nullptr);rankingStudyRunner_->clear();}
    if(rankingFuturesFeed_){disconnect(rankingFuturesFeed_,nullptr,this,nullptr);rankingFuturesFeed_->clear();}
    if(marketBreadth_){disconnect(marketBreadth_,nullptr,this,nullptr);marketBreadth_->setEnabled(false);}
    if(rapidMoves_)disconnect(rapidMoves_,nullptr,this,nullptr);
    if(chartStudyFeed_){disconnect(chartStudyFeed_,nullptr,this,nullptr);chartStudyFeed_->clearOwner("explore-relative");chartStudyFeed_->clearOwner("explore-ranking");}
    dashboardRefreshing_=true;todayDashboard_=nullptr;exploreSummaryHeader_=nullptr;exploreCoinSummary_=nullptr;
    if(dashboardOverview_)disconnect(dashboardOverview_,nullptr,this,nullptr);
    if(dashboardNotices_)disconnect(dashboardNotices_,nullptr,this,nullptr);
    // Child views are deleted after LocalWindow members; their last snapshot must not reach freed state.
    if(marketMetricsView_)disconnect(marketMetricsView_,nullptr,this,nullptr);
    if(todayBtcPremium_){disconnect(todayBtcPremium_,nullptr,this,nullptr);todayBtcPremium_->setActive(false);}
    if(worldMarkets_){disconnect(worldMarkets_,nullptr,this,nullptr);worldMarkets_->setActive(false);}
    if(marketFlowHistory_){disconnect(marketFlowHistory_,nullptr,this,nullptr);marketFlowHistory_->setActive(false);}
    kiwoomStopping_=true;
    if(chartKiwoomStream_)disconnect(chartKiwoomStream_,nullptr,this,nullptr);
    revokeKiwoomMarketAuthorization();
    chartCoinFlowStopping_=true;
    if(chartCoinFlowRefresh_)chartCoinFlowRefresh_->stop();
    if(chartCoinFlow_)disconnect(chartCoinFlow_,nullptr,this,nullptr);
    if(chartFlowMinutes_)disconnect(chartFlowMinutes_,nullptr,this,nullptr);
    if(chartFlowMinutes_)chartFlowMinutes_->setSelection({},60000);
    if(chartCoinFlow_)chartCoinFlow_->stop();
    if(chartAlertFeed_)chartAlertFeed_->setRunning(false);
    if(chartStockSource_)disconnect(chartStockSource_,nullptr,this,nullptr);
    if(alertStockSource_)disconnect(alertStockSource_,nullptr,this,nullptr);
    revokeStockChartAuthorization();
    if(chartStockSource_)chartStockSource_->setActive(false);
    if(chartAlerts_)disconnect(chartAlerts_,nullptr,this,nullptr);
    if(tray_){disconnect(tray_,nullptr,this,nullptr);tray_->setMenu(nullptr);tray_->hide();}
    chartLists_=nullptr;chartWorkspace_=nullptr;closeSensitiveDialog();
}


void LocalWindow::setQuitPreparation(std::function<void(std::function<void(bool)>)> prepare,std::function<void()> cancel) {
    quitPreparation_=std::move(prepare);quitCancellation_=std::move(cancel);
    if(auto* app=QCoreApplication::instance())app->installEventFilter(this);
}
bool LocalWindow::eventFilter(QObject* watched,QEvent* event) {
    // Native/application Quit must use the same bounded preparation as menu/close.
    if(watched==QCoreApplication::instance()&&event->type()==QEvent::Quit&&quitPreparation_&&!quitting_){requestQuit();return true;}
    return QMainWindow::eventFilter(watched,event);
}
void LocalWindow::setReleaseUpdater(ReleaseUpdater* updater, std::function<bool(QString*)> startInstaller) {
    releaseUpdater_ = updater;
    startUpdateInstaller_ = std::move(startInstaller);
    connect(updater, &ReleaseUpdater::archiveReady, this, [this] {
        if (quitPreparing_ || quitting_) return;
        restartingForUpdate_ = true;
        requestQuit();
    });
}

void LocalWindow::requestQuit() {
    if(quitting_||quitPreparing_)return;
    if(quitPreparation_){
        quitPreparing_=true;
        feedback_->setText(tr("종료 중이에요."));
        const QPointer<LocalWindow> guard(this);
        auto* deadline=new QTimer(this);deadline->setSingleShot(true);deadline->setTimerType(Qt::PreciseTimer);
        // A timeout can cancel a writer that also completes its callback. A late
        // completion must never finish the next update attempt.
        const auto completed = std::make_shared<bool>(false);
        const auto finish=[guard,deadline,completed](bool saved){
            if(!guard || std::exchange(*completed, true))return;
            QTimer::singleShot(0,guard.data(),[guard,deadline,saved]{
                if(!guard||!guard->quitPreparing_||guard->quitting_)return;
                deadline->stop();deadline->deleteLater();guard->quitPreparing_=false;
                guard->finishPreparedQuit(saved);
            });
        };
        connect(deadline,&QTimer::timeout,this,[guard,finish]{
            if(!guard||!guard->quitPreparing_||guard->quitting_)return;
            finish(false);
            if(guard->quitCancellation_)guard->quitCancellation_();
        });
        deadline->start(3000);quitPreparation_(finish);
        return;
    }
    finishPreparedQuit(true);
}

void LocalWindow::finishPreparedQuit(bool saved) {
    if (restartingForUpdate_ && !saved) {
        restartingForUpdate_ = false;
        feedback_->clear();
        releaseUpdater_->refuse(tr("작업을 저장하지 못해 업데이트를 중단했습니다. 다시 시도해 주세요."));
        return;
    }
    lockAndClear();
    if (restartingForUpdate_) {
        restartingForUpdate_ = false;
        QString error;
        if (!startUpdateInstaller_(&error)) {
            releaseUpdater_->refuse(error.isEmpty() ? tr("업데이트 설치를 시작할 수 없습니다.") : error);
            return;
        }
        releaseUpdater_->handoffComplete();
    }
    quitting_ = true;
    emit quitRequested();
    close();
}

void LocalWindow::closeEvent(QCloseEvent* event) {
    // Public display preference, independent of the vault and its close policy.
    ProfileSettings().setValue("display/windowSize/v1", size());
    // The saved background/close policy lives in the deferred alerts panel.
    // An early close must honor it even before the first gate paint.
    initializeWorkbenchOnce();
    closeSensitiveDialog();

    if (assetsWindow_) assetsWindow_->close();
    const auto decision=backgroundCloseDecision(quitting_,background_->isChecked(),tray_&&tray_->available(),tray_&&tray_->visible(),chartAlertsPanel_?chartAlertsPanel_->closeHintSeen():closeHintSeen_);
    if (decision.action==BackgroundCloseAction::Hide) {
        if(decision.showExplanation){
            closeHintSeen_=true;if(chartAlertsPanel_)chartAlertsPanel_->markCloseHintSeen();
            QMessageBox::information(this,tr("메뉴 막대에서 계속 실행"),tr("창을 닫아도 메뉴 막대에서 알림을 확인할 수 있어요. 옵션 > 일반에서 백그라운드 실행을 끄거나 메뉴에서 종료할 수 있어요."));
        }
        hide();
        event->ignore();
        return;
    }
    if (!quitting_) {
        if(quitPreparation_){event->ignore();requestQuit();return;}
        quitting_ = true;
        lockAndClear();
        emit quitRequested();
    }
    event->accept();
}
void LocalWindow::paintEvent(QPaintEvent* event) {
    QMainWindow::paintEvent(event);
    if(!vaultGate_||!vaultGate_->isVisible()||!deferredWorkbenchInitializer_||workbenchInitializationQueued_)return;
    workbenchInitializationQueued_=true;
    // Keep the first gate paint ahead of view construction. QObject context
    // cancels the queued call if the window is destroyed before it runs.
    QTimer::singleShot(0,this,&LocalWindow::initializeWorkbenchOnce);
}

void LocalWindow::showEvent(QShowEvent* event){
    QMainWindow::showEvent(event);
    windowShownOnce_=true;
    syncMarketFlowBackground();
    flushPortfolioLoadingProjection();
    if(portfolioAssetsDirty_)renderPortfolioAssets();
    syncChartPresentation();
    QTimer::singleShot(0,this,&LocalWindow::syncChartPresentation);
}
void LocalWindow::hideEvent(QHideEvent* event){clearAssetShare();QMainWindow::hideEvent(event);if(todayBtcPremium_)todayBtcPremium_->setActive(false);if(worldMarkets_)worldMarkets_->setActive(false);syncChartPresentation();}


void LocalWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    syncHeaderTabs();
    syncConnectionLayout();
    if(portfolioAllocation_)QTimer::singleShot(0,this,&LocalWindow::syncPortfolioLayout);
    QTimer::singleShot(0, this, [this] { layoutSpotlight(); }); // After the nested widgets took their new size.
}

void LocalWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        flushPortfolioLoadingProjection();
        if(portfolioAssetsDirty_)renderPortfolioAssets();
        QTimer::singleShot(0, this, &LocalWindow::syncChartPresentation);
    }
}

void LocalWindow::syncHeaderTabs() {
    auto* pages = findChild<QTabWidget*>("portfolioPages");
    if (!pages || !headerTabs_) return;
    auto* layout = headerTabs_->layout();
    auto buttons = headerTabs_->findChildren<QPushButton*>(QString(), Qt::FindDirectChildrenOnly);
    if (buttons.size() != pages->count()) {
        for (auto* old : buttons) delete old;
        buttons.clear();
        for (int i = 0; i < pages->count(); ++i) {
            auto* tab = new QPushButton(headerTabs_);
            tab->setObjectName(QStringLiteral("headerTab%1").arg(i));
            tab->setProperty("headerTab", true);
            tab->setCheckable(true);
            tab->setAutoExclusive(true);
            tab->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            connect(tab, &QPushButton::toggled, this, [pages, i](bool selected) {
                if (selected) pages->setCurrentIndex(i);
            });
            layout->addWidget(tab);
            buttons.append(tab);
        }
    }
    for (int i = 0; i < buttons.size(); ++i) {
        // At the minimum window width the theme switch also needs room.
        // Reduce only horizontal tab padding; keep every label and control.
        const bool compact=width()<ListDetailView::NarrowWidth;
        if(buttons[i]->property("compactHeader").toBool()!=compact){
            buttons[i]->setProperty("compactHeader",compact);
            buttons[i]->style()->unpolish(buttons[i]);
            buttons[i]->style()->polish(buttons[i]);
            buttons[i]->updateGeometry();
        }
        const auto label = pages->tabText(i);
        if (buttons[i]->text() != label) buttons[i]->setText(label);
        if (buttons[i]->accessibleName() != label) buttons[i]->setAccessibleName(label);
        buttons[i]->setChecked(i == pages->currentIndex());
    }
    headerTabs_->setVisible(pages->count() > 1);
    if(auto* heading=findChild<QHBoxLayout*>("workbenchHeading")){
        auto* view=findChild<QComboBox*>("exploreView");
        const bool today=pages->currentWidget()&&pages->currentWidget()->objectName()=="explorePage"&&view&&view->currentData()=="flow";
        const int inset=today?qMax(6,(centralWidget()->width()-36-ExploreMaximumWidth)/2):0;
        heading->setContentsMargins(inset,0,inset,0);
    }
}


void LocalWindow::applyLocalTheme() {
    setPalette(KeefiTheme::palette());
    setFont(KeefiFonts::font(FontRole::Body15));
    setStyleSheet(KeefiTheme::localStyleSheet(this));
    if (optionsDialog_) optionsDialog_->setStyleSheet(styleSheet());
}

} // namespace keefi
