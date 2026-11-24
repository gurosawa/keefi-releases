#include "keefi/AccountDialog.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/LedgerTextLabel.h"
#include "keefi/ConnectionPresentation.h"
#include "keefi/CompactControls.h"
#include "keefi/AccountModel.h"
#include "keefi/LocalComboBox.h"
#include "keefi/SegmentedComboBox.h"
#include "keefi/PressRevealButton.h"
#include "keefi/SecretInput.h"
#include "keefi/SecureBuffer.h"
#include "keefi/PortfolioController.h"
#include "keefi/HyperliquidReadController.h"
#include "keefi/SolanaTransactionCodec.h"

#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QFormLayout>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <stdexcept>

namespace keefi {
namespace {
constexpr int CredentialByteLimit = 8192;
// Regional endpoints and choices remain available for a future explicit rollout.
constexpr bool OkxRegionSelectionEnabled = false;
QString choiceLabel(const QString& value) {
    if (value == "live" || value == "real" || value == "production") return QObject::tr("실전");
    if (value == "paper" || value == "demo" || value == "mock") return QObject::tr("모의");
    if (value == "testnet") return QObject::tr("테스트넷");
    if (value == "mainnet") return QObject::tr("메인넷");
    if (value == "domestic_equity" || value == "domestic" || value == "domestic_stock" || value == "kr_stock" || value == "krx")
        return QObject::tr("국내주식");
    if (value == "spot") return QObject::tr("현물");
    if (value == "usdm_futures" || value == "usd_m_futures" || value == "usdm" || value == "usd-m") return QObject::tr("USDⓈ-M 선물");
    if (value == "okx_futures") return QObject::tr("SWAP·FUTURES");
    if (value == "hyperliquid_perps") return QObject::tr("무기한 선물(Hyperliquid)");
    if (value == "solana") return QObject::tr("Solana 지갑(현물)");
    if (value == "swap") return QObject::tr("Solana 지갑(현물)");
    return value;
}
// Identify only the address encoding supported by the two read-only wallet adapters.
QString phantomAddressMarket(const QString& address) {
    if (address.size() == 42 && HyperliquidReadController::validAddress(address))
        return QStringLiteral("hyperliquid_perps");
    if (address.size() >= 32 && address.size() <= 44 && SolanaTransactionCodec::publicKeyValid(address))
        return QStringLiteral("solana");
    return {};
}

void fillChoices(QComboBox* box, const QStringList& choices) {
    box->clear();
    for (const auto& choice : choices)
        box->addItem(choiceLabel(choice), choice);
}
// A completed connection should not inherit the hidden credential form's
// minimum height. The active page alone defines the wizard's layout hints.
class AccountStepStack final : public QStackedWidget {
  public:
    explicit AccountStepStack(QWidget* parent) : QStackedWidget(parent) {
        layout()->setSizeConstraint(QLayout::SetNoConstraint);
        connect(this, &QStackedWidget::currentChanged, this, [this] { updateGeometry(); });
    }
    QSize sizeHint() const override {
        return currentWidget() ? currentWidget()->sizeHint() : QSize{};
    }
    QSize minimumSizeHint() const override {
        return currentWidget() ? currentWidget()->minimumSizeHint() : QSize{};
    }
    bool hasHeightForWidth() const override {
        return currentWidget() && currentWidget()->hasHeightForWidth();
    }
    int heightForWidth(int width) const override {
        return currentWidget() ? currentWidget()->heightForWidth(width) : -1;
    }
};

// One service per tile: its initial in a quiet disc, then its name. A click,
// Space or Return opens the service's fields at once.
class ServiceTile final : public QAbstractButton {
  public:
    static constexpr int Height = 48;
    ServiceTile(const QString& initial, const QString& label, QWidget* parent)
        : QAbstractButton(parent), initial_(initial) {
        setText(label);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(Height);
        setFont(KeefiFonts::font(FontRole::BodyStrong15));
        bindThemePresentation(this, [this] { update(); });
    }
    QSize sizeHint() const override { return {180, Height}; }
    QSize minimumSizeHint() const override { return {120, Height}; }

  protected:
    void focusInEvent(QFocusEvent* event) override {
        // The ring marks keyboard focus only. Opening the window must not
        // look as if the first service were already chosen.
        const auto reason = event->reason();
        keyboardFocus_ = reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason || reason == Qt::ShortcutFocusReason;
        QAbstractButton::focusInEvent(event);
    }
    void focusOutEvent(QFocusEvent* event) override {
        keyboardFocus_ = false;
        QAbstractButton::focusOutEvent(event);
    }
    void keyPressEvent(QKeyEvent* event) override {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && !event->isAutoRepeat()) {
            click();
            event->accept();
            return;
        }
        QAbstractButton::keyPressEvent(event);
    }
    void paintEvent(QPaintEvent*) override {
        const auto& c = themeColors();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool ring = hasFocus() && keyboardFocus_;
        const bool lifted = isDown() || (underMouse() && isEnabled());
        const qreal border = ring ? 2.0 : 1.0;
        painter.setPen(QPen(ring ? c.accent : c.rule, border));
        painter.setBrush(isDown() ? c.rule : lifted ? c.surface : c.bg);
        painter.drawRoundedRect(QRectF(rect()).adjusted(border / 2, border / 2, -border / 2, -border / 2), 8, 8);
        const QRectF disc(12, (height() - 26) / 2.0, 26, 26);
        painter.setPen(Qt::NoPen);
        painter.setBrush(lifted ? c.bg : c.surface);
        painter.drawEllipse(disc);
        painter.setFont(KeefiFonts::font(FontRole::CaptionStrong12));
        painter.setPen(c.text2);
        painter.drawText(disc, Qt::AlignCenter, initial_);
        const QFont labelFont = KeefiFonts::font(FontRole::BodyStrong15);
        const int left = int(disc.right()) + 10;
        const QRect labelRect(left, 0, qMax(0, width() - left - 12), height());
        painter.setFont(labelFont);
        painter.setPen(isEnabled() ? c.text : c.text3);
        painter.drawText(labelRect, Qt::AlignVCenter | Qt::AlignLeft,
                         QFontMetrics(labelFont).elidedText(text(), Qt::ElideRight, labelRect.width()));
    }

  private:
    QString initial_;
    bool keyboardFocus_ = false;
};
} // namespace

AccountDialog::AccountDialog(bool persistentAvailable, QWidget* parent)
    : AccountDialog(persistentAvailable, parent, false) {}

AccountDialog::AccountDialog(bool persistentAvailable, QWidget* parent, bool stockChartOrigin)
    : QDialog(parent), persistentAvailable_(persistentAvailable), stockChartOrigin_(stockChartOrigin) {
    setObjectName("accountDialog");
    setWindowTitle(stockChartOrigin_ ? tr("증권 연결하기") : tr("계좌 연결"));
    setModal(true);
    setMinimumWidth(540);
    setMinimumHeight(420);
    const auto* screen = QGuiApplication::primaryScreen();
    resize(600, screen ? qMax(420, qMin(680, screen->availableGeometry().height() - 80)) : 640);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(20,18,20,18);
    outer->setSpacing(14);
    auto* stepStrip = new QFrame(this);
    stepStrip->setObjectName("accountConnectionStepStrip");
    auto* stepLayout = new QHBoxLayout(stepStrip);
    stepLayout->setContentsMargins(0, 0, 0, 0);
    stepLayout->setSpacing(16);
    const QStringList stepNames{tr("서비스 선택"), tr("연결 정보"), tr("연결 확인")};
    for (int index = 0; index < int(stepLabels_.size()); ++index) {
        auto* label = new QLabel(stepNames[index], stepStrip);
        label->setFont(KeefiFonts::font(FontRole::Secondary14));
        label->setFixedHeight(22);
        label->setAccessibleName(tr("%1 / 3 · %2").arg(index + 1).arg(label->text()));
        stepLabels_[index] = label;
        stepLayout->addWidget(label);
    }
    stepLabels_[0]->setObjectName("accountConnectionStep");
    stepLayout->addStretch();
    setThemeStyle(stepStrip, QStringLiteral(R"(
        QFrame#accountConnectionStepStrip { border-bottom: 1px solid ${rule}; }
        QLabel { color: ${text3}; border-bottom: 2px solid transparent; }
        QLabel[accountCurrentStep="true"] { color: ${text}; border-bottom-color: ${accent}; font-weight: 600; }
    )"));
    outer->addWidget(stepStrip);
    steps_=new AccountStepStack(this);steps_->setObjectName("accountConnectionSteps");outer->addWidget(steps_,1);
    // Nine services fit on one page, so they are tiles: no search and no
    // separate "next" step. A tile opens the service's fields directly.
    auto* serviceScroll = new QScrollArea(steps_);
    serviceScroll->setObjectName("accountServiceScroll");
    serviceScroll->setFrameShape(QFrame::NoFrame);
    serviceScroll->setWidgetResizable(true);
    serviceScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* servicePage = new QWidget(serviceScroll);
    servicePage->setObjectName("accountServiceList");
    servicePage->setAccessibleName(tr("연결할 서비스"));
    auto* serviceLayout = new QVBoxLayout(servicePage);
    serviceLayout->setContentsMargins(0, 0, 0, 0);
    serviceLayout->setSpacing(20);
    auto* serviceHeading = new QLabel(windowTitle(), servicePage);
    serviceHeading->setObjectName("accountServiceHeading");
    serviceHeading->setProperty("type", "section");
    serviceLayout->addWidget(serviceHeading);
    using ServiceGroups = QList<QPair<QString, QStringList>>;
    const ServiceGroups categories = stockChartOrigin_
        ? ServiceGroups{{tr("새 증권 연결"), {"toss", "kiwoom"}}}
        : ServiceGroups{{tr("거래소"), {"binance", "upbit", "okx", "bithumb"}},
                        {tr("증권"), {"toss", "kiwoom", "kis", "ls"}}, {tr("지갑"), {"phantom"}}};
    if (stockChartOrigin_) {
        existingChartAccounts_ = new QVBoxLayout;
        existingChartAccounts_->setSpacing(6);
        serviceLayout->addLayout(existingChartAccounts_);
    }
    const QHash<QString, QString> initials{{"binance", "B"}, {"upbit", "U"}, {"okx", "O"}, {"bithumb", tr("빗")},
        {"toss", tr("토")}, {"kis", tr("한")}, {"kiwoom", tr("키")}, {"ls", "LS"}, {"phantom", "P"}};
    // One grid for every group keeps the three columns the same width, also
    // in a group with a single tile.
    auto* grid = new QGridLayout;
    grid->setSpacing(8);
    const int columns = stockChartOrigin_ ? 2 : 3;
    for (int column = 0; column < columns; ++column)
        grid->setColumnStretch(column, 1);
    int row = 0;
    for (const auto& [category, ids] : categories) {
        auto* groupLabel = new QLabel(category, servicePage);
        groupLabel->setFont(KeefiFonts::font(FontRole::Caption12));
        groupLabel->setProperty("serviceGroup", true);
        if (row > 0) groupLabel->setContentsMargins(0, 12, 0, 0); // 20 px between groups with the grid's 8
        grid->addWidget(groupLabel, row++, 0, 1, columns);
        int placed = 0;
        for (const auto& id : ids) {
            const auto* spec = AccountModel::provider(id);
            if (!spec) continue;
            auto* tile = new ServiceTile(initials.value(id, spec->label.left(1)), spec->label, servicePage);
            tile->setObjectName("accountService_" + id);
            const bool available = AccountModel::allowsNewConnection(id);
            tile->setEnabled(available);
            tile->setAccessibleName(available ? tr("%1 연결").arg(spec->label) : tr("%1 · 연결 비활성").arg(spec->label));
            if (!available) {
                tile->setText(tr("%1 · 비활성").arg(spec->label));
                tile->setToolTip(tr("이 증권사의 계좌 연결과 조회는 중단되었습니다."));
            }
            tile->setAccessibleDescription(category);
            connect(tile, &QAbstractButton::clicked, this, [this, id] { chooseService(id); });
            grid->addWidget(tile, row + placed / columns, placed % columns);
            ++placed;
        }
        row += (placed + columns - 1) / columns;
    }
    serviceLayout->addLayout(grid);
    if (stockChartOrigin_) {
        auto* coverage = new QLabel(tr("토스증권은 국내·미국 주식 차트를, 키움증권은 국내 주식 일봉을 제공합니다."), servicePage);
        coverage->setWordWrap(true);
        serviceLayout->addWidget(coverage);
    }
    serviceLayout->addStretch();
    setThemeStyle(servicePage, QStringLiteral("QLabel[serviceGroup=\"true\"] { color: ${text3}; }"));
    serviceScroll->setWidget(servicePage);
    steps_->addWidget(serviceScroll);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName("accountFormScroll");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* contents = new QWidget(scroll);
    auto* layout = new QVBoxLayout(contents);
    layout->setAlignment(Qt::AlignTop);
    scroll->setWidget(contents);
    steps_->addWidget(scroll);
    selectedService_=new QLabel(contents);selectedService_->setObjectName("accountSelectedService");selectedService_->setProperty("type","section");layout->addWidget(selectedService_);
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    layout->addLayout(form);
    auto* providerSelector = new LocalComboBox(this);
    providerSelector->setCompact();
    provider_ = providerSelector;
    provider_->setObjectName("providerSelector");
    for (const auto& provider : AccountModel::providers())
        if (!AccountModel::isProviderSuspended(provider.id)) provider_->addItem(provider.label, provider.id);
    provider_->hide(); // Canonical provider IDs; the service tiles control this model.
    alias_ = new QLineEdit(this);
    alias_->setObjectName("accountAlias");
    alias_->setMaxLength(80);
    alias_->setPlaceholderText(tr("예: 내 국내주식 계좌"));
    form->addRow(tr("계좌 별칭 (선택)"), alias_);
    environment_ = new SegmentedComboBox(this);
    environment_->setObjectName("environmentSelector");
    form->addRow(tr("환경"), environment_);
    form->setRowVisible(environment_, false); // Keep the canonical live/mainnet value without a single-choice row.
    region_ = new SegmentedComboBox(this);
    region_->setObjectName("okxRegionSelector");
    region_->addItem(tr("지역을 선택하세요"), QString{});
    region_->addItem(tr("Global"), "global");
    region_->addItem(tr("미국·호주 등록 계정"), "us");
    region_->addItem(tr("EEA 등록 계정"), "eea");
    regionLabel_ = new QLabel(tr("OKX 계정 등록 지역"), this);
    form->addRow(regionLabel_, region_);
    market_ = new SegmentedComboBox(this);
    market_->setObjectName("marketSelector");
    form->addRow(tr("시장"), market_);
    currency_ = new SegmentedComboBox(this);
    currency_->setObjectName("currencySelector");
    form->addRow(tr("기준 통화"), currency_);
    // Account metadata stays intact; holdings valuation is always KRW in this build.
    form->setRowVisible(currency_, false);
    auto* credentialSource = new LocalComboBox(this);
    credentialSource->setCompact();
    // Keep the existing full-width credential source field.
    credentialSource->setMaximumWidth(QWIDGETSIZE_MAX);
    credentialSource->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    credentialSource_ = credentialSource;
    credentialSource_->setObjectName("credentialSourceSelector");
    credentialSourceLabel_ = new QLabel(tr("Binance 인증정보"), this);
    form->addRow(credentialSourceLabel_, credentialSource_);
    sharedCredentialHint_ = new LedgerTextLabel(FontRole::Secondary14, this);
    sharedCredentialHint_->setObjectName("sharedCredentialHint");
    sharedCredentialHint_->setTextFormat(Qt::PlainText);
    sharedCredentialHint_->setWordWrap(true);
    layout->addWidget(sharedCredentialHint_);
    guide_ = new QLabel(this);
    guide_->setObjectName("providerGuide");
    guide_->setTextFormat(Qt::RichText);
    guide_->setFont(KeefiFonts::font(FontRole::Secondary14));
    guide_->setOpenExternalLinks(true);
    guide_->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    guide_->setWordWrap(true);
    auto* guideActions = new QHBoxLayout;
    issueKey_ = new QPushButton(tr("공식 사이트에서 키 발급 ↗"), this);
    issueKey_->setObjectName("providerIssueKey");
    issueKey_->setAutoDefault(false);
    connect(issueKey_, &QPushButton::clicked, this, [this] {
        const auto* spec = AccountModel::provider(provider_->currentData().toString());
        if (spec && !spec->issuanceUrl.isEmpty() && !QDesktopServices::openUrl(QUrl(spec->issuanceUrl)))
            error_->setText(tr("기본 브라우저에서 키 발급 페이지를 열 수 없습니다."));
    });
    guideActions->addWidget(guide_);
    guideActions->addWidget(issueKey_);
    guideActions->addStretch();
    layout->addLayout(guideActions);
    credentialsWidget_ = new QWidget(this);
    credentialsForm_ = new QFormLayout(credentialsWidget_);
    credentialsForm_->setContentsMargins(0, 0, 0, 0);
    credentialsForm_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    layout->addWidget(credentialsWidget_);
    addressFeedback_ = new QLabel(this);
    addressFeedback_->setObjectName("phantomAddressFeedback");
    addressFeedback_->setTextFormat(Qt::PlainText);
    addressFeedback_->setWordWrap(true);
    layout->addWidget(addressFeedback_);
    switchAddressMarket_ = new QPushButton(this);
    switchAddressMarket_->setObjectName("switchPhantomAddressMarket");
    switchAddressMarket_->setAutoDefault(false);
    layout->addWidget(switchAddressMarket_, 0, Qt::AlignLeft);
    connect(switchAddressMarket_, &QPushButton::clicked, this, [this] {
        const auto* address = inputs_.value("address");
        if (!address || replacing_) return;
        const int target = market_->findData(phantomAddressMarket(address->text()));
        if (target >= 0) market_->setCurrentIndex(target);
    });
    trimNotice_ = new LedgerTextLabel(FontRole::Secondary14, this);
    trimNotice_->setObjectName("credentialTrimNotice");
    trimNotice_->setTextFormat(Qt::PlainText);
    trimNotice_->setWordWrap(false);
    // The hidden notice keeps its slot, so a paste that trims spaces moves nothing below it.
    // Its only text is set up front: an empty label is 1px shorter than the shown one.
    trimNotice_->setText(tr("앞뒤 공백을 제거했습니다."));
    auto trimPolicy = trimNotice_->sizePolicy();
    trimPolicy.setRetainSizeWhenHidden(true);
    trimNotice_->setSizePolicy(trimPolicy);
    trimNotice_->hide();
    layout->addWidget(trimNotice_);
    // Connections are stored only in the encrypted vault; there is no in-memory-only mode.
    storageHint_ = new LedgerTextLabel(FontRole::Secondary14, this);
    storageHint_->setObjectName("storageHint");
    storageHint_->setWordWrap(true);
    layout->addWidget(storageHint_);
    openVault_ = new QPushButton(tr("금고 열기"), this);
    openVault_->setObjectName("openVaultForAccount");
    openVault_->setVisible(!persistentAvailable_);
    layout->addWidget(openVault_);
    connect(openVault_, &QPushButton::clicked, this, &AccountDialog::vaultRequested);
    status_ = new LedgerTextLabel(FontRole::Body15, this);
    status_->setObjectName("accountStageStatus");
    status_->setWordWrap(true);
    layout->addWidget(status_);
    error_ = new QLabel(this);
    error_->setObjectName("accountError");
    error_->setTextFormat(Qt::PlainText);
    error_->setProperty("tone", "error");
    error_->setWordWrap(true);
    layout->addWidget(error_);
    auto* buttons = new QDialogButtonBox(this);
    back_=buttons->addButton(tr("‹ 서비스 선택"),QDialogButtonBox::ActionRole);back_->setObjectName("accountServiceBack");
    submit_ = buttons->addButton(tr("저장"), QDialogButtonBox::AcceptRole);
    submit_->setObjectName("saveAccount");
    submit_->setProperty("primary", true);
    auto* cancel = buttons->addButton(tr("취소"), QDialogButtonBox::RejectRole);
    cancel->setObjectName("cancelAccount");
    finish_=buttons->addButton(tr("연결 상태 보기"),QDialogButtonBox::ActionRole);finish_->setObjectName("accountConnectionFinish");finish_->setProperty("primary",true);
    for(auto* b:{back_,submit_,cancel,finish_})b->setAutoDefault(false);
    outer->addWidget(buttons);
    resultScroll_=new QScrollArea(steps_);resultScroll_->setObjectName("accountResultScroll");
    resultScroll_->setFrameShape(QFrame::NoFrame);resultScroll_->setWidgetResizable(true);
    resultScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* resultPage=new QWidget(resultScroll_);auto* resultLayout=new QVBoxLayout(resultPage);
    resultLayout->setContentsMargins(0,6,0,6);resultLayout->setSpacing(12);
    resultLayout->setSizeConstraint(QLayout::SetMinAndMaxSize);
    result_=new QLabel(resultPage);result_->setObjectName("accountConnectionResult");result_->setTextFormat(Qt::AutoText);result_->setWordWrap(true);
    result_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    result_->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Preferred);
    result_->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);resultLayout->addWidget(result_);
    retry_=new QPushButton(tr("다시 조회"),resultPage);retry_->setObjectName("accountConnectionRetry");resultLayout->addWidget(retry_,0,Qt::AlignLeft);
    resultLayout->addStretch();resultScroll_->setWidget(resultPage);steps_->addWidget(resultScroll_);
    connect(back_,&QPushButton::clicked,this,[this]{clearSensitiveInputs();showStep(0);
        // Return to the tile of the service just left.
        if(auto* tile=findChild<QAbstractButton*>("accountService_"+provider_->currentData().toString()))tile->setFocus();});
    connect(finish_,&QPushButton::clicked,this,[this]{
        if (stockChartOrigin_) {
            returnToChart();
            return;
        }
        if(resultHasAssets_)emit assetsRequested(resultConnectionId_);else emit connectionRequested(resultConnectionId_);
        accept();
    });
    connect(retry_,&QPushButton::clicked,this,[this]{if(!resultPortfolio_||resultConnectionId_.isEmpty())return;
        if(resultPortfolio_->canReconnectConnection(resultConnectionId_))resultPortfolio_->reconnectConnection(resultConnectionId_);
        else resultPortfolio_->refreshConnection(resultConnectionId_);refreshConnectionResult();});
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (!submit_->isEnabled() || steps_->currentIndex()==2)
            return;
        AccountDraft draft;
        draft.provider = provider_->currentData().toString();
        // An empty alias takes the service's name.
        draft.alias = alias_->text().trimmed().isEmpty() ? provider_->currentText() : alias_->text().trimmed();
        draft.environment = environment_->currentData().toString();
        draft.market = market_->currentData().toString();
        draft.currency = draft.provider == "phantom" && draft.market == "hyperliquid_perps"
            ? QStringLiteral("USDC") : currency_->currentData().toString();
        draft.region = draft.provider == "okx" ? okxRegionForSubmission() : QString{};
        draft.sourceAccountId = credentialSource_->currentData().toString();
        if (draft.sourceAccountId.isEmpty()) {
            // The saved value must equal the masked value on screen.
            for (auto* input : std::as_const(inputs_))
                normalizeCredential(input);
            if (!submit_->isEnabled())
                return; // A whitespace-only required value is now empty.
        }
        if (draft.sourceAccountId.isEmpty())
            for (auto it = inputs_.cbegin(); it != inputs_.cend(); ++it)
                draft.credentials.insert(it.key(), it.value()->text());
        emit submitted(draft);
        draft.credentials.clear();
    });
    connect(provider_, &QComboBox::currentIndexChanged, this, &AccountDialog::rebuildProvider);
    connect(provider_, &QComboBox::currentIndexChanged, this, [this]{showStep(1);});
    connect(alias_, &QLineEdit::textChanged, this, &AccountDialog::updateSubmitState);
    connect(region_, &QComboBox::currentIndexChanged, this, &AccountDialog::updateSubmitState);
    connect(market_, &QComboBox::currentIndexChanged, this, [this] {
        // Rebuilding the form normally clears credentials. Retain a public address
        // only when the newly selected wallet market accepts its exact encoding.
        QString retained;
        if (provider_->currentData() == "phantom") {
            if (const auto* field = inputs_.value("address"); field && phantomAddressMarket(field->text()) == market_->currentData().toString())
                retained = field->text();
        }
        updateEnvironments();
        if (!retained.isEmpty()) inputs_.value("address")->setText(retained);
    });
    connect(environment_, &QComboBox::currentIndexChanged, this, &AccountDialog::updateCredentialSources);
    connect(credentialSource_, &QComboBox::currentIndexChanged, this, &AccountDialog::updateCredentialSourceSelection);
    rebuildProvider();
    for (auto* control : findChildren<SegmentedComboBox*>()) control->setCompact();
    for (auto* control : findChildren<QPushButton*>()) applyCompactButton(control);
    applyCompactInput(alias_);
    bindThemePresentation(this,[this]{refreshGuide();});
    showStep(0);
}

AccountDialog::~AccountDialog() { clearSensitiveInputs(); }

void AccountDialog::setPersistentAvailable(bool available) {
    persistentAvailable_ = available;
    openVault_->setVisible(!available && !replacing_);
    // Do not rebuild the credential-source selector here: it clears draft inputs.
    updateSubmitState();
}

void AccountDialog::setReplacement(const AccountDraft& account) {
    if (AccountModel::isProviderSuspended(account.provider)) {
        provider_->setCurrentIndex(-1);
        reject();
        return;
    }
    replacing_ = true;
    replacementRegion_ = account.region;
    openVault_->hide();
    provider_->setCurrentIndex(provider_->findData(account.provider));
    alias_->setText(account.alias);
    market_->setCurrentIndex(market_->findData(account.market));
    updateEnvironments();
    if (environment_->findData(account.environment) < 0)
        environment_->addItem(tr("기존 환경 %1 · 신규 등록 불가").arg(choiceLabel(account.environment)), account.environment);
    environment_->setCurrentIndex(environment_->findData(account.environment));
    currency_->setCurrentIndex(currency_->findData(account.currency));
    region_->setCurrentIndex(qMax(0, region_->findData(account.region)));
    for (auto* box : {provider_, environment_, market_, currency_})
        box->setEnabled(false);
    alias_->setReadOnly(true);
    setWindowTitle(account.provider == "phantom" ? tr("지갑 주소 변경") : tr("API 키 변경"));
    submit_->setText(tr("변경 저장"));
    clearSensitiveInputs();
    updateCredentialSources();
    status_->setText(tr("새 인증정보를 입력하세요. 저장 후 다시 조회합니다."));
    if (account.provider == "okx") {
        region_->setEnabled(OkxRegionSelectionEnabled);
        if (OkxRegionSelectionEnabled) {
            setWindowTitle(tr("지역·API 키 변경"));
            status_->setText(tr("등록 지역과 새 API 키를 확인하세요. Passphrase의 앞뒤 공백은 그대로 사용합니다."));
        } else {
            const int registered = region_->findData(account.region);
            status_->setText(registered > 0
                ? tr("기존 등록 지역: %1 · 지역은 변경하지 않습니다.\n새 API 키를 입력하세요. Passphrase의 앞뒤 공백은 그대로 사용합니다.").arg(region_->itemText(registered))
                : tr("기존 등록 지역 미확인 · 이 연결의 API 키를 변경할 수 없습니다. 기존 연결 정보는 유지됩니다."));
        }
    }
    updateMarketForm();
    updateSubmitState();
    showStep(1);
}

void AccountDialog::showStep(int step) {
    steps_->setCurrentIndex(step);
    for (int index = 0; index < int(stepLabels_.size()); ++index) {
        auto* label = stepLabels_[index];
        const bool current = index == (replacing_ ? 0 : step);
        label->setVisible(!replacing_ || index == 0);
        label->setProperty("accountCurrentStep", current);
        label->style()->unpolish(label);
        label->style()->polish(label);
    }
    if (replacing_) {
        stepLabels_[0]->setText(windowTitle());
        stepLabels_[0]->setAccessibleName(windowTitle());
    }
    // A new connection shows where it is by its content (the list, the
    // service's fields, the result); the step names stay for accessibility.
    stepLabels_[0]->parentWidget()->setVisible(replacing_);
    selectedService_->setText(provider_->currentText());
    back_->setVisible(step==1&&!replacing_);submit_->setVisible(step==1);finish_->setVisible(step==2);
    findChild<QPushButton*>("cancelAccount")->setVisible(step!=2);
    if(step==2)submit_->setEnabled(false);
    if(step==2)fitConnectionResult();
    submit_->setDefault(step==1);finish_->setDefault(step==2);
}

void AccountDialog::chooseService(const QString& id) {
    if (!AccountModel::allowsNewConnection(id) || (stockChartOrigin_ && id != "toss" && id != "kiwoom")) return;
    const int index = provider_->findData(id);
    if (index < 0) return;
    // Choosing the service that is already current changes no index, so the
    // step is shown here as well as by the selector's change signal.
    provider_->setCurrentIndex(index);
    showStep(1);
    alias_->setFocus();
}

void AccountDialog::showConnectionResult(const QString& connectionId, PortfolioController* portfolio) {
    clearSensitiveInputs();resultConnectionId_=connectionId;resultPortfolio_=portfolio;showStep(2);
    if(portfolio){connect(portfolio,&PortfolioController::changed,this,&AccountDialog::refreshConnectionResult);
        connect(portfolio,&PortfolioController::aboutToReset,this,[this]{result_->clear();retry_->hide();});}
    refreshConnectionResult();
}

void AccountDialog::refreshConnectionResult() {
    if(steps_->currentIndex()!=2)return;
    QString state=tr("조회 대기"),detail=tr("잔고 조회를 기다리고 있습니다.");bool retry=false;
    resultHasAssets_=false;
    if(resultPortfolio_)for(const auto& c:resultPortfolio_->connections())if(c.account.id==resultConnectionId_){
        if (const auto wallet = phantomWalletReadResult(c, *resultPortfolio_)) {
            state = wallet->state;
            detail = wallet->detail;
            resultHasAssets_ = wallet->hasAssets;
            retry = !c.active && (c.state == "stopped" ||
                (c.state == "failed" && (c.account.market == "solana" || resultPortfolio_->canReconnectConnection(c.account.id))));
            retry_->setText(tr("다시 조회"));
            retry_->setToolTip({});
            break;
        }
        if(c.active){state=tr("잔고 조회 중");detail=tr("완료되면 이 화면에 표시됩니다.");}
        else if(c.state=="complete"){
            state=tr("잔고 조회 완료");
            resultHasAssets_=c.account.environment=="live"||c.account.environment=="mainnet";
            detail=resultHasAssets_?QString{}:tr("모의·테스트넷 조회 결과입니다.");
            if (stockChartOrigin_ && !chartReturnQueued_ && !chartReturnSent_) {
                chartReturnQueued_ = true;
                // Let the save/read handler and any pending modal setup finish first.
                QTimer::singleShot(0, this, [this] {
                    chartReturnQueued_ = false;
                    if (isVisible() && steps_->currentIndex() == 2 && resultPortfolio_) {
                        for (const auto& connection : resultPortfolio_->connections())
                            if (connection.account.id == resultConnectionId_ && !connection.active && connection.state == "complete") {
                                returnToChart();
                                break;
                            }
                    }
                });
            }
        }
        else if(connectionNeedsAttention(c)){
            const bool reconnect = resultPortfolio_->canReconnectConnection(resultConnectionId_);
            state = connectionStateName(c); detail = c.stage; retry = c.state == "stopped" || reconnect;
            retry_->setText(reconnect ? (c.account.market=="domestic_equity"?tr("다시 인증"):tr("다시 연결")) : tr("다시 조회"));
            retry_->setToolTip(c.account.market=="domestic_equity"?tr("같은 인증정보를 쓰는 다른 앱의 세션에 영향을 줄 수 있습니다."):QString{});
        }else if(!c.stage.isEmpty())detail=c.stage;
        break;
    }
    // Confirm the save in two words; how the key is stored is in the user guide.
    const auto text=state+(detail.isEmpty()?QString{}:"\n"+detail)+(stockChartOrigin_ ? QString{} : tr("\n\n저장 완료"));
    // Native rich-text layout owns both glyph positions and selection hit
    // testing. Escape every original character before adding font-only spans.
    const auto renderedText=KeefiFonts::mixedTextHtml(text,FontRole::Body15);
    const bool geometryChanged=result_->text()!=renderedText||retry_->isHidden()==retry;
    result_->setText(renderedText);retry_->setVisible(retry);retry_->setEnabled(retry);
    finish_->setText(stockChartOrigin_ ? tr("보던 차트로 돌아가기") : resultHasAssets_ ? tr("보유 자산 보기") : tr("연결 상태 보기"));
    if(geometryChanged)fitConnectionResult();
}

void AccountDialog::returnToChart() {
    if (chartReturnSent_ || resultConnectionId_.isEmpty()) return;
    chartReturnSent_ = true;
    emit chartReturnRequested(resultConnectionId_);
    accept();
}

void AccountDialog::fitConnectionResult() {
    if(steps_->currentIndex()!=2)return;
    ensurePolished();
    const auto* activeScreen=screen();
    const auto available=activeScreen?activeScreen->availableGeometry().size():QSize(1024,768);
    const int availableWidth=qMax(240,available.width()-40);
    const int availableHeight=qMax(160,available.height()-80);
    const int targetWidth=qMin(600,availableWidth);
    setMinimumWidth(qMin(540,availableWidth));
    setMinimumHeight(qMin(200,availableHeight));
    const auto margins=layout()->contentsMargins();
    const int contentWidth=targetWidth-margins.left()-margins.right()-16;
    const int resultHeight=qMax(result_->fontMetrics().height(),result_->heightForWidth(contentWidth));
    const int retryHeight=retry_->isHidden()?0:retry_->sizeHint().height()+12;
    const int contentHeight=resultHeight+retryHeight+12;
    const auto* strip=stepLabels_[0]->parentWidget();
    const int chromeHeight=margins.top()+margins.bottom()+(strip->isHidden()?0:strip->sizeHint().height())
        +finish_->sizeHint().height()+2*layout()->spacing();
    const int targetHeight=qBound(qMin(200,availableHeight),contentHeight+chromeHeight,qMin(420,availableHeight));
    steps_->updateGeometry();
    layout()->invalidate();
    layout()->activate();
    resize(targetWidth,targetHeight);
}

void AccountDialog::setCredentialSources(const QList<AccountRecord>& accounts) {
    credentialSources_ = accounts;
    updateCredentialSources();
    if (!existingChartAccounts_) return;
    while (auto* item = existingChartAccounts_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (const auto& account : accounts) {
        if ((account.provider != "toss" && account.provider != "kiwoom") || account.environment != "live"
            || account.market != "domestic_equity" || account.id.isEmpty()) continue;
        if (existingChartAccounts_->count() == 0)
            existingChartAccounts_->addWidget(new QLabel(tr("이미 연결한 증권 계좌"), this));
        const auto label = AccountModel::provider(account.provider)->label;
        const auto alias = account.alias.isEmpty() || account.alias == label
            ? label : tr("%1 · %2").arg(label, account.alias);
        // Escape ampersands so account aliases are rendered literally by QPushButton.
        auto* button = new QPushButton(QString(alias).replace('&', "&&"), this);
        button->setAccessibleName(tr("%1 계좌로 차트 보기").arg(alias));
        button->setObjectName("existingChartConnection_" + account.id);
        button->setAutoDefault(false);
        applyCompactButton(button);
        connect(button, &QPushButton::clicked, this, [this, id = account.id] { emit existingChartConnectionRequested(id); });
        existingChartAccounts_->addWidget(button);
    }
}

void AccountDialog::setSharedCredentialUsers(const QList<AccountRecord>& accounts) {
    if (accounts.size() <= 1) return;
    QStringList affected;
    for (const auto& account : accounts) affected.append(tr("%1 · %2").arg(account.alias, choiceLabel(account.market)));
    sharedCredentialHint_->setText(tr("API 키 변경은 다음 연결 모두에 적용됩니다.\n%1").arg(affected.join('\n')));
    sharedCredentialHint_->show();
}

void AccountDialog::updateEnvironments() {
    const auto* provider = AccountModel::provider(provider_->currentData().toString());
    if (!provider) return;
    const auto previous = environment_->currentData().toString();
    {
        const QSignalBlocker block(environment_);
        fillChoices(environment_, provider->environments); // Live (Solana mainnet) only since 2026-10-21.
        environment_->setCurrentIndex(qMax(0, environment_->findData(previous)));
    }
    updateCredentialSources();
    updateMarketForm();
}

void AccountDialog::updateMarketForm() {
    const auto* provider = AccountModel::provider(provider_->currentData().toString());
    if (!provider) return;
    const bool phantom = provider->id == "phantom";
    const bool perps = phantom && market_->currentData() == "hyperliquid_perps";
    currency_->setEnabled(!replacing_ && !perps);
    if (!phantom) return;
    const auto previous = currency_->currentData().toString();
    {
        const QSignalBlocker block(currency_);
        fillChoices(currency_, perps ? QStringList{"USDC"} : provider->currencies);
        currency_->setCurrentIndex(qMax(0, currency_->findData(previous)));
    }
    if (auto* address = inputs_.value("address")) {
        if (auto* label = qobject_cast<QLabel*>(credentialsForm_->labelForField(address->parentWidget())))
            label->setText(perps ? tr("조회용 Ethereum 주소") : tr("조회용 Solana 주소"));
        address->setPlaceholderText(perps ? tr("0x로 시작하는 40자리 hex 주소") : tr("조회할 Solana 지갑 주소"));
    }
    status_->setText(perps
        ? tr("Hyperliquid 무기한 선물 잔고·포지션을 조회할 Ethereum 주소를 입력하세요. 0x와 40자리 hex 주소가 필요합니다. 개인키·시드 문구·서명은 받지 않습니다.")
        : tr("조회할 Solana 지갑 주소를 입력하세요. 개인키·시드 문구는 입력하지 않습니다."));
    updateSubmitState();
}

QString AccountDialog::okxRegionForSubmission() const {
    if (OkxRegionSelectionEnabled) return region_->currentData().toString();
    return replacing_ ? replacementRegion_ : QStringLiteral("global");
}

void AccountDialog::updateCredentialSources() {
    if (!credentialSource_ || !trimNotice_ || !submit_) return;
    const auto previous = credentialSource_->currentData().toString();
    const bool allowed = !replacing_ && provider_->currentData() == "binance" && environment_->currentData() == "live";
    {
        const QSignalBlocker block(credentialSource_);
        credentialSource_->clear();
        credentialSource_->addItem(tr("새 API Key·Secret 입력"), QString{});
        if (allowed) for (const auto& source : credentialSources_) {
            if (source.provider != "binance" || source.environment != "live" || source.market == market_->currentData() ||
                (source.market != "spot" && source.market != "usdm_futures") ||
                !persistentAvailable_) continue;
            credentialSource_->addItem(tr("저장된 키 사용 · %1 · %2").arg(source.alias, choiceLabel(source.market)), source.id);
        }
        credentialSource_->setCurrentIndex(qMax(0, credentialSource_->findData(previous)));
    }
    credentialSource_->setVisible(allowed && credentialSource_->count() > 1);
    credentialSourceLabel_->setVisible(allowed && credentialSource_->count() > 1);
    updateCredentialSourceSelection();
}

void AccountDialog::updateCredentialSourceSelection() {
    if (!credentialsWidget_ || !trimNotice_) return;
    const auto sourceId = credentialSource_->currentData().toString();
    const bool shared = !sourceId.isEmpty();
    clearSensitiveInputs();
    credentialsWidget_->setVisible(!shared);
    if (shared) {
        sharedCredentialHint_->setText(tr("같은 계좌 아래 시장을 추가합니다. 잔고는 시장별로 조회합니다."));
    } else if (!replacing_) sharedCredentialHint_->clear();
    sharedCredentialHint_->setVisible(!sharedCredentialHint_->text().isEmpty());
    updateSubmitState();
}

void AccountDialog::setError(const QString& message) {
    error_->setText(message);
    clearSensitiveInputs();
}

void AccountDialog::clearSensitiveInputs() {
    for (auto* reveal : reveals_)
        reveal->conceal();
    if (trimNotice_)
        trimNotice_->hide();
    for (auto* input : inputs_) {
        input->setEchoMode(QLineEdit::Password);
        input->setText(QString()); // setText also discards QLineEdit undo/redo history.
    }
}

void AccountDialog::done(int result) {
    clearSensitiveInputs();
    QDialog::done(result);
}

void AccountDialog::refreshGuide() {
    const auto* provider = AccountModel::provider(provider_->currentData().toString());
    if (!provider) return;
    const QString guideText = !provider->issuanceUrl.isEmpty() ? tr("%1 연결 가이드 ↗").arg(provider->label)
                             : provider->keyless ? tr("%1 주소 복사 가이드 ↗").arg(provider->label) : tr("키 만드는 법 ↗");
    issueKey_->setVisible(!provider->issuanceUrl.isEmpty());
    issueKey_->setAccessibleName(tr("%1 공식 사이트에서 키 발급").arg(provider->label));
    guide_->setText(themeStyle(QStringLiteral("<a href=\"%1\" style=\"color:${accentText}\">%2</a>"))
        .arg(provider->guideUrl.toHtmlEscaped(), guideText.toHtmlEscaped()));
}

bool AccountDialog::importKiwoomKeyFiles(const QStringList& paths) {
    if (provider_->currentData() != "kiwoom" || !inputs_.contains("app_key") || !inputs_.contains("app_secret"))
        return false;
    const auto rejectImport = [this](const QString& message) {
        error_->setText(message);
        return false;
    };
    if (paths.size() != 2)
        return rejectImport(tr("App Key 파일과 App Secret 파일을 하나씩 함께 선택하세요."));

    // Kiwoom downloads <account>_appkey.txt and <account>_appsecret.txt.
    // Browsers can append a copy number before the extension.
    static const QRegularExpression namePattern(QStringLiteral(R"(^(.+)_(appkey|appsecret)(?:\s*\(\d+\))?\.txt$)"),
                                                QRegularExpression::CaseInsensitiveOption);
    QString account;
    CredentialSet values;
    for (const auto& path : paths) {
        const QFileInfo info(path);
        const auto match = namePattern.match(info.fileName());
        if (!match.hasMatch())
            return rejectImport(tr("키움에서 받은 계좌번호_appkey.txt와 계좌번호_appsecret.txt를 선택하세요."));
        if (!account.isEmpty() && account != match.captured(1))
            return rejectImport(tr("두 파일의 계좌가 다릅니다. 같은 계좌에서 발급한 두 파일을 선택하세요."));
        account = match.captured(1);
        const QString field = match.captured(2).compare("appkey", Qt::CaseInsensitive) == 0 ? "app_key" : "app_secret";
        if (values.contains(field))
            return rejectImport(tr("같은 종류의 파일이 두 개입니다. App Key와 App Secret을 하나씩 선택하세요."));
        QFile file(path);
        if (!info.isFile() || !file.open(QIODevice::ReadOnly | QIODevice::Unbuffered))
            return rejectImport(tr("선택한 파일을 읽을 수 없습니다. 파일의 위치와 읽기 권한을 확인하세요."));
        const auto size = file.size();
        if (size <= 0 || size > CredentialByteLimit + 32)
            return rejectImport(tr("키 파일을 읽을 수 없거나 허용 크기를 초과했습니다."));
        try {
            SecureBuffer raw(static_cast<std::size_t>(size));
            auto* data = reinterpret_cast<char*>(raw.data());
            if (file.read(data, size) != size || !file.atEnd() || file.error() != QFile::NoError)
                return rejectImport(tr("키 파일을 끝까지 읽을 수 없습니다."));
            QByteArrayView value(data, size);
            if (value.startsWith("\xef\xbb\xbf")) value = value.sliced(3);
            value = value.trimmed();
            if (value.isEmpty() || value.size() > CredentialByteLimit ||
                std::any_of(value.begin(), value.end(), [](unsigned char ch) { return ch < 0x21 || ch > 0x7e; }))
                return rejectImport(tr("키 파일에 한 줄의 키 값이 필요합니다. 키움에서 받은 원본 파일을 확인하세요."));
            values.emplace(field, SecureBuffer(value.data(), static_cast<std::size_t>(value.size())));
        } catch (const std::exception&) {
            return rejectImport(tr("키 파일을 읽을 보호 메모리를 확보할 수 없습니다."));
        }
    }
    // Leave both existing fields intact until both files have passed validation.
    for (auto* reveal : std::as_const(reveals_)) reveal->conceal();
    for (const auto& [field, value] : values)
        inputs_.value(field)->setText(QString::fromLatin1(reinterpret_cast<const char*>(value.data()), value.size()));
    error_->clear();
    status_->setText(tr("키 파일 2개를 불러왔습니다. 저장을 누르면 금고에 보관하고 연결을 확인합니다."));
    return true;
}

void AccountDialog::rebuildProvider() {
    clearSensitiveInputs();
    inputs_.clear();
    required_.clear();
    reveals_.clear(); // The rows below own the buttons.
    while (credentialsForm_->rowCount() > 0)
        credentialsForm_->removeRow(0);
    const auto* provider = AccountModel::provider(provider_->currentData().toString());
    if (!provider) {
        updateSubmitState();
        return;
    }
    { const QSignalBlocker block(market_); fillChoices(market_, provider->markets); }
    updateEnvironments();
    fillChoices(currency_, provider->currencies);
    region_->setCurrentIndex(OkxRegionSelectionEnabled ? 0 : region_->findData("global"));
    region_->setVisible(provider->id == "okx" && OkxRegionSelectionEnabled);
    regionLabel_->setVisible(provider->id == "okx" && OkxRegionSelectionEnabled);
    refreshGuide();
    if (!replacing_) alias_->setPlaceholderText(provider->label);
    if (provider->id == "kiwoom") {
        auto* import = new QPushButton(tr("키 파일 2개 불러오기"), credentialsWidget_);
        import->setObjectName("importKiwoomKeyFiles");
        import->setAutoDefault(false);
        applyCompactButton(import);
        credentialsForm_->addRow(import);
        connect(import, &QPushButton::clicked, this, [this] {
            const auto paths = QFileDialog::getOpenFileNames(this, tr("키움 App Key와 App Secret 파일 선택"),
                QStandardPaths::writableLocation(QStandardPaths::DownloadLocation), tr("키움 키 파일 (*.txt);;모든 파일 (*)"));
            if (!paths.isEmpty()) importKiwoomKeyFiles(paths);
        });
    }
    for (const auto& field : provider->fields) {
        auto* edit = new QLineEdit(credentialsWidget_);
        edit->setObjectName("credential_" + field.key);
        applyCompactInput(edit);
        edit->setEchoMode(QLineEdit::Password);
        edit->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
        // Keep an over-limit sentinel; never silently save a truncated valid prefix.
        edit->setMaxLength(CredentialByteLimit + 1);
        QString fieldLabel = field.label;
        if (provider->id == "kiwoom" && field.key == "account_number") {
            fieldLabel = tr("일치 확인용 계좌번호"); edit->setPlaceholderText(tr("숫자 10자리 · 하이픈 없이 · 앞자리 0 보존"));
        }
        if (trimsCredentialField(field.key)) {
            // Issued keys and secrets drop leading/trailing whitespace on paste,
            // on finishing input and again right before saving.
            edit->setProperty("keefiTrimWhiteSpace", true);
            edit->installEventFilter(this);
            connect(edit, &QLineEdit::editingFinished, this, [this, edit] { normalizeCredential(edit); });
        }
        auto* row = new QWidget(credentialsWidget_);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->addWidget(edit, 1);
        auto* reveal = new PressRevealButton(edit, row);
        reveal->setObjectName("reveal_" + field.key);
        reveal->setProperty("secretReveal", true);
        reveal->setToolTip(tr("누르는 동안 입력값 표시"));
        reveal->setInlineEye();
        reveals_.append(reveal);
        credentialsForm_->addRow(fieldLabel + (field.required ? QString() : tr(" (선택)")), row);
        inputs_.insert(field.key, edit);
        if (field.required)
            required_.append(field.key);
        connect(edit, &QLineEdit::textChanged, this, &AccountDialog::updateSubmitState);
    }

        static const QHash<QString, QString> hints{
            {"binance", tr("HMAC API Key·Secret을 입력하세요.")},
            {"upbit", tr("자산 조회 권한이 있는 API 키를 입력하세요.")},
            {"bithumb", tr("자산 조회 권한이 있는 API 키를 입력하세요.")},
            {"okx", OkxRegionSelectionEnabled
                ? tr("계정 등록 지역을 선택하세요. Passphrase의 앞뒤 공백은 그대로 사용합니다.")
                : tr("현재 OKX 신규 연결은 Global 등록 계정만 지원합니다. Passphrase의 앞뒤 공백은 그대로 사용합니다.")},
            {"toss", tr("공식 IP 허용 설정을 확인하세요.")},
            {"kiwoom", tr("공식 IP 허용 설정을 확인하세요. 선택 계좌번호는 일치 확인용이며 다른 계좌를 지정하지 않습니다. 계좌 비밀번호는 받지 않습니다.")},
            {"phantom", tr("조회할 Solana 지갑 주소를 입력하세요. 개인키·시드 문구는 입력하지 않습니다.")},
            {"jupiter", tr("현재 버전에서는 이 연결의 자산 조회를 지원하지 않습니다.")}
        };
        status_->setText(hints.value(provider->id));

    error_->clear();
    updateCredentialSources();
    updateMarketForm();
    updateSubmitState();
}

bool AccountDialog::updatePhantomAddressFeedback() {
    const bool phantom = provider_->currentData() == "phantom";
    auto* field = phantom ? inputs_.value("address") : nullptr;
    const QString address = field ? field->text() : QString{};
    const auto detected = phantomAddressMarket(address);
    const bool matches = !detected.isEmpty() && detected == market_->currentData().toString();
    const bool canSwitch = !detected.isEmpty() && !matches && !replacing_;
    QString message;
    if (!address.isEmpty()) {
        const bool solana = market_->currentData() == "solana";
        if (matches)
            message = solana ? tr("Solana 공개 주소 형식이 맞아요.") : tr("Ethereum 주소 형식이 맞아요. Hyperliquid 조회에 사용할 수 있어요.");
        else if (detected.isEmpty())
            message = solana ? tr("Solana 공개 주소를 확인해 주세요. 팬텀의 Solana 항목에서 주소 전체를 복사하세요.")
                             : tr("Ethereum 주소를 확인해 주세요. 팬텀의 Ethereum 항목에서 0x로 시작하는 주소 전체를 복사하세요.");
        else
            message = solana ? tr("Ethereum 주소 형식이에요. Solana 현물은 팬텀의 Solana 주소로 조회해요.")
                             : tr("Solana 주소 형식이에요. Hyperliquid 선물은 팬텀의 Ethereum 주소로 조회해요.");
    }
    addressFeedback_->setText(message);
    addressFeedback_->setVisible(!message.isEmpty());
    if (field) field->setAccessibleDescription(message);
    if (canSwitch)
        switchAddressMarket_->setText(detected == "solana" ? tr("Solana 현물로 전환") : tr("Hyperliquid 선물로 전환"));
    switchAddressMarket_->setVisible(canSwitch);
    return !phantom || matches;
}

void AccountDialog::updateSubmitState() {
    if (!submit_)
        return;
    const auto* provider = AccountModel::provider(provider_->currentData().toString());
    bool ready = provider && AccountModel::allowsNewConnection(provider->id)
        && (!stockChartOrigin_ || provider->id == "toss" || provider->id == "kiwoom")
        && (!provider->keyless || !provider->fields.isEmpty());
    const bool walletAddressReady = updatePhantomAddressFeedback();
    ready = ready && walletAddressReady;
    if (provider && provider->id == "okx") {
        const auto region = okxRegionForSubmission();
        ready = ready && (region == "global" || region == "us" || region == "eea");
    }
    // A required OKX region with no choice yet is outlined in red.
    const bool regionMissing = provider && provider->id == "okx" && okxRegionForSubmission().isEmpty();
    if (region_->property("error").toBool() != regionMissing) {
        region_->setProperty("error", regionMissing);
        region_->update();
    }
    bool oversized = false;
    for (auto* input : inputs_)
        oversized = oversized || exceedsSecretByteLimit(input->text(), CredentialByteLimit);
    if (oversized) {
        error_->setText(tr("인증정보는 항목마다 UTF-8 8192바이트 이하여야 합니다. 너무 긴 입력은 저장하지 않습니다."));
        error_->setProperty("sizeError", true);
    } else if (error_->property("sizeError").toBool()) {
        error_->clear();
        error_->setProperty("sizeError", false);
    }
    if (credentialSource_->currentData().toString().isEmpty())
        for (const auto& field : required_) {
            const auto* input = inputs_.value(field);
            ready = ready && !(input->property("keefiTrimWhiteSpace").toBool()
                                   ? trimUnicodeWhiteSpace(input->text()) : input->text()).isEmpty();
        }
    ready = ready && persistentAvailable_;
    if (!replacing_) {
        submit_->setText(tr("저장하고 연결"));
        storageHint_->setText(persistentAvailable_
            ? tr("이 PC의 암호화 금고에 저장합니다.")
            : tr("암호화 금고에만 저장합니다. 먼저 금고를 열어 주세요."));
        // Shown only when it blocks saving; where keys are kept is in the user guide.
        storageHint_->setVisible(!persistentAvailable_);
        openVault_->setVisible(!persistentAvailable_ && credentialSource_->currentData().toString().isEmpty());
    }
    submit_->setEnabled(ready && !oversized);
}

void AccountDialog::normalizeCredential(QLineEdit* edit) {
    if (!edit || !edit->property("keefiTrimWhiteSpace").toBool())
        return;
    const QString trimmed = trimUnicodeWhiteSpace(edit->text());
    if (trimmed.size() == edit->text().size())
        return;
    edit->setText(trimmed);
    trimNotice_->setText(tr("앞뒤 공백을 제거했습니다."));
    trimNotice_->show();
}

// Trims the whole candidate (current text with the selection replaced by the
// clipboard), never only the pasted fragment, so interior characters stay exact.
bool AccountDialog::pasteTrimmed(QLineEdit* edit) {
    if (edit->isReadOnly())
        return false;
    const QString text = edit->text();
    const int start = edit->hasSelectedText() ? edit->selectionStart() : edit->cursorPosition();
    const int length = edit->hasSelectedText() ? edit->selectionLength() : 0;
    const QString inserted = QGuiApplication::clipboard()->text();
    const QString candidate = text.left(start) + inserted + text.mid(start + length);
    const QString trimmed = trimUnicodeWhiteSpace(candidate);
    edit->setText(trimmed);
    const qsizetype leading = candidate.indexOf(trimmed.isEmpty() ? candidate : trimmed);
    edit->setCursorPosition(int(qBound<qsizetype>(0, start + inserted.size() - qMax<qsizetype>(leading, 0), trimmed.size())));
    if (trimmed.size() != candidate.size()) {
        trimNotice_->setText(tr("앞뒤 공백을 제거했습니다."));
        trimNotice_->show();
    }
    return true;
}

bool AccountDialog::eventFilter(QObject* watched, QEvent* event) {
    auto* edit = qobject_cast<QLineEdit*>(watched);
    if (edit && event->type() == QEvent::KeyPress && edit->property("keefiTrimWhiteSpace").toBool() &&
        static_cast<QKeyEvent*>(event)->matches(QKeySequence::Paste) && pasteTrimmed(edit))
        return true;
    return QDialog::eventFilter(watched, event);
}

} // namespace keefi
