#include "ProfileUpgradeDialog.h"
#include "CompactControls.h"
#include "ProductIdentity.h"
#include "VaultDialog.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>
#include <utility>

namespace keefi {
namespace {
QLabel* text(QVBoxLayout* layout, QWidget* parent, const QString& value, const QString& name,
             FontRole role = FontRole::Body15) {
    auto* label = new QLabel(value, parent);
    label->setObjectName(name);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setFont(KeefiFonts::font(role));
    layout->addWidget(label);
    return label;
}
void decorate(QDialog* dialog, const QString& title) {
    dialog->setWindowTitle(devApplicationName() + QStringLiteral(" · ") + title);
    dialog->setFont(KeefiFonts::font(FontRole::Body15));
    dialog->setMinimumWidth(520);
    dialog->resize(640, 360);
    setThemeStyle(dialog, QStringLiteral(
        "QDialog { background: ${bg}; color: ${text}; } QLabel, QRadioButton { color: ${text}; }"));
}
}

ProfileUpgradeDialog::ProfileUpgradeDialog(const ProfileUpgradePlan& plan, QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("profileUpgradeDialog"));
    decorate(this, QStringLiteral("프로필 전환"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);
    const bool choosing = plan.action == ProfileUpgradeAction::ChooseSource;
    text(layout, this, choosing ? QStringLiteral("전환할 원본을 선택하세요")
                               : QStringLiteral("프로필을 새 형식으로 전환할까요?"),
         QStringLiteral("profileUpgradeTitle"), FontRole::Title17);
    text(layout, this,
         QStringLiteral("원본을 보존하고 별도 폴더에서 금고와 설정을 새 형식으로 전환합니다. "
                        "검증이 끝난 사본만 사용합니다. 취소하면 기존 자료는 그대로 남습니다."),
         QStringLiteral("profileUpgradeDescription"));
    auto pending = std::make_shared<ProfileUpgradeSourceKind>(choosing
        ? ProfileUpgradeSourceKind::None : plan.sourceKind);
    text(layout, this, QStringLiteral("원본 프로필"), QStringLiteral("profileUpgradeSourceCaption"), FontRole::Caption12);
    auto* sourcePath = text(layout, this, choosing ? QStringLiteral("원본을 선택하세요") : plan.sourceDirectory,
                            QStringLiteral("profileUpgradeSourcePath"));
    sourcePath->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    QList<QRadioButton*> choices;
    if (choosing) {
        for (const auto& source : plan.sources) {
            const bool renamed = source.kind == ProfileUpgradeSourceKind::Dev100Keefi;
            auto* radio = new QRadioButton((renamed ? QStringLiteral("이름 변경 후 프로필")
                                                    : QStringLiteral("이전 프로필"))
                                           + QStringLiteral(" — ") + source.directory, this);
            radio->setObjectName(renamed ? QStringLiteral("profileUpgradeSource_renamed")
                                        : QStringLiteral("profileUpgradeSource_legacy"));
            radio->setChecked(false);
            layout->addWidget(radio);
            choices.append(radio);
            connect(radio, &QRadioButton::toggled, this, [pending, sourcePath, source](bool checked) {
                if (checked) { *pending = source.kind; sourcePath->setText(source.directory); }
            });
        }
    }
    text(layout, this, QStringLiteral("새 프로필"), QStringLiteral("profileUpgradeTargetCaption"), FontRole::Caption12);
    auto* target = text(layout, this, plan.targetDirectory, QStringLiteral("profileUpgradeTargetPath"));
    target->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    layout->addStretch();
    auto* actions = new QHBoxLayout;
    actions->addStretch();
    auto* cancel = new QPushButton(QStringLiteral("취소"), this);
    cancel->setObjectName(QStringLiteral("profileUpgradeCancel"));
    auto* proceed = new QPushButton(QStringLiteral("계속"), this);
    proceed->setObjectName(QStringLiteral("profileUpgradeContinue"));
    for (auto* button : {cancel, proceed}) {
        applyCompactButton(button);
        button->setAutoDefault(false);
        button->setDefault(false);
        actions->addWidget(button);
    }
    proceed->setEnabled((plan.action == ProfileUpgradeAction::OfferUpgrade
                         || plan.action == ProfileUpgradeAction::ChooseSource)
                        && *pending != ProfileUpgradeSourceKind::None);
    for (auto* choice : choices)
        connect(choice, &QRadioButton::toggled, this, [pending, proceed] {
            proceed->setEnabled(*pending != ProfileUpgradeSourceKind::None);
        });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(proceed, &QPushButton::clicked, this, [this, pending] {
        if (*pending == ProfileUpgradeSourceKind::None) return;
        source_ = *pending;
        accept();
    });
    layout->addLayout(actions);
    cancel->setFocus();
}
ProfileUpgradeSourceKind ProfileUpgradeDialog::sourceKind() const { return source_; }
void ProfileUpgradeDialog::done(int result) {
    if (result != QDialog::Accepted) source_ = ProfileUpgradeSourceKind::None;
    QDialog::done(result);
}

struct ProfileUpgradePasswordDialog::Private {
    ProfileUpgradePlan plan;
    QString version;
    ProfileStageConverter converter;
    std::unique_ptr<ProfileUpgradeSession> session;
    VaultDialog* form = nullptr;
    ProfileReady ready;
    QString error;
    bool privateAccess = false;
    bool preparationFailed = false;
    bool prepare() {
        error.clear();
        session.reset();
        session = ProfileUpgradeSession::begin(plan, version, &error);
        if (!session || !session->copy(&error)) {
            session.reset();
            preparationFailed = true;
            return false;
        }
        preparationFailed = false;
        return true;
    }
};
ProfileUpgradePasswordDialog::ProfileUpgradePasswordDialog(
    const ProfileUpgradePlan& plan, const QString& version, ProfileStageConverter converter,
    QWidget* parent) : QDialog(parent), d_(std::make_unique<Private>()) {
    setObjectName(QStringLiteral("profileUpgradePasswordDialog"));
    decorate(this, QStringLiteral("프로필 전환 암호 확인"));
    d_->plan = plan;
    d_->version = version;
    d_->converter = std::move(converter);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);
    text(layout, this, QStringLiteral("기존 금고 암호를 입력하세요"),
         QStringLiteral("profileUpgradePasswordTitle"), FontRole::Title17);
    text(layout, this,
         QStringLiteral("원본은 보존합니다. 복사본의 암호화 자료를 확인하고 새 형식으로 전환한 뒤 새 프로필을 사용합니다."),
         QStringLiteral("profileUpgradePasswordDescription"));
    d_->form = new VaultDialog(VaultDialog::Mode::Unlock, this, VaultDialog::Presentation::Embedded);
    d_->form->setObjectName(QStringLiteral("profileUpgradePasswordForm"));
    layout->addWidget(d_->form);
    auto* cancel = new QPushButton(QStringLiteral("취소"), this);
    cancel->setObjectName(QStringLiteral("profileUpgradePasswordCancel"));
    applyCompactButton(cancel);
    cancel->setAutoDefault(false);
    layout->addWidget(cancel, 0, Qt::AlignRight);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(d_->form, &VaultDialog::submitted, this, [this](const QString& password, const QString&) {
        if (!d_->privateAccess || d_->preparationFailed) {
            d_->form->clearSensitiveInputs();
            return;
        }
        if (!d_->session && !d_->prepare()) {
            d_->form->setError(d_->error);
            d_->form->setEnabled(false);
            return;
        }
        QString error;
        if (!d_->session->convert(password, d_->converter, &error)
            || !d_->session->verify(&error) || !d_->session->publish(&error)) {
            d_->session.reset();
            d_->error = error.isEmpty() ? QStringLiteral("프로필 전환을 마치지 못했습니다.") : error;
            d_->form->setError(d_->error);
            d_->form->setEnabled(d_->privateAccess && !d_->preparationFailed);
            return;
        }
        d_->ready = d_->session->ready();
        d_->error.clear();
        d_->session.reset();
        d_->form->clearSensitiveInputs();
        accept();
    });
    if (!d_->prepare()) d_->form->setError(d_->error);
    d_->form->setEnabled(false);
}
ProfileUpgradePasswordDialog::~ProfileUpgradePasswordDialog() {
    if (d_->form) d_->form->clearSensitiveInputs();
    d_->session.reset();
}
void ProfileUpgradePasswordDialog::setPrivateAccess(bool allowed) {
    d_->privateAccess = allowed;
    if (!allowed) d_->form->clearSensitiveInputs();
    d_->form->setEnabled(allowed && !d_->preparationFailed);
}
ProfileReady ProfileUpgradePasswordDialog::ready() const { return d_->ready; }
QString ProfileUpgradePasswordDialog::error() const { return d_->error; }
void ProfileUpgradePasswordDialog::lockAndClear(const QString& reason) {
    d_->privateAccess = false;
    d_->error = reason;
    d_->form->clearSensitiveInputs();
    reject();
}
void ProfileUpgradePasswordDialog::done(int result) {
    if (result == QDialog::Accepted && d_->ready.directory.isEmpty()) result = QDialog::Rejected;
    d_->form->clearSensitiveInputs();
    d_->session.reset();
    if (result != QDialog::Accepted) d_->ready = {};
    QDialog::done(result);
}
} // namespace keefi
