#include "keefi/ProfileMigrationDialog.h"
#include "keefi/CompactControls.h"
#include "keefi/ProductIdentity.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace keefi {

ProfileMigrationDialog::ProfileMigrationDialog(const ProfileMigrationPlan& plan, QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("profileMigrationDialog"));
    setWindowTitle(devApplicationName() + QStringLiteral(" · 프로필 선택"));
    setFont(KeefiFonts::font(FontRole::Body15));
    setMinimumWidth(500);
    resize(620, 320);
    setThemeStyle(this, QStringLiteral("QDialog { background: ${bg}; color: ${text}; } QLabel { color: ${text}; }"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);
    const auto addText = [this, layout](const QString& text, const QString& name,
                                      FontRole role = FontRole::Body15) {
        auto* label = new QLabel(text, this);
        label->setObjectName(name);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setFont(KeefiFonts::font(role));
        layout->addWidget(label);
        return label;
    };

    const bool offerCopy = plan.action == DevProfileAction::OfferCopy;
    const bool chooseExisting = plan.action == DevProfileAction::ChooseExisting;
    addText(offerCopy ? QStringLiteral("프로필을 새 이름으로 옮길까요?")
                      : QStringLiteral("사용할 프로필을 확인하세요"),
            QStringLiteral("profileMigrationTitle"), FontRole::Title17);
    addText(offerCopy
                ? QStringLiteral("금고와 설정을 새 폴더로 복사합니다. 옛 폴더는 지우지 않습니다.")
                : chooseExisting
                    ? QStringLiteral("옛 프로필과 새 프로필이 모두 있습니다. 사용할 프로필을 선택하세요. 여기서는 파일을 복사하거나 지우지 않습니다.")
                    : QStringLiteral("프로필 경로를 확인하세요. 이 안내 창은 파일을 복사하거나 지우지 않습니다."),
            QStringLiteral("profileMigrationDescription"));

    addText(QStringLiteral("옛 프로필"), QStringLiteral("profileMigrationSourceCaption"), FontRole::Caption12);
    auto* source = addText(plan.paths.legacyDirectory, QStringLiteral("profileMigrationSourcePath"));
    source->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    addText(QStringLiteral("새 프로필"), QStringLiteral("profileMigrationDestinationCaption"), FontRole::Caption12);
    auto* destination = addText(plan.paths.currentDirectory, QStringLiteral("profileMigrationDestinationPath"));
    destination->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    auto* reason = addText(plan.error, QStringLiteral("profileMigrationReason"));
    reason->setVisible(!plan.error.isEmpty());
    layout->addStretch();

    if (offerCopy || chooseExisting) {
        auto* actions = new QHBoxLayout;
        actions->setSpacing(8);
        actions->addStretch();
        const auto addAction = [this, actions](const QString& name, const QString& text,
                                               ProfileMigrationChoice choice) {
            auto* button = new QPushButton(text, this);
            button->setObjectName(name);
            applyCompactButton(button);
            button->setAutoDefault(false);
            button->setDefault(false);
            connect(button, &QPushButton::clicked, this, [this, choice] {
                choice_ = choice;
                accept();
            });
            actions->addWidget(button);
            return button;
        };
        if (offerCopy) {
            auto* later = addAction(QStringLiteral("profileMigrationLater"), QStringLiteral("나중에"),
                                    ProfileMigrationChoice::Later);
            addAction(QStringLiteral("profileMigrationCopy"), QStringLiteral("옮기기"),
                      ProfileMigrationChoice::Copy);
            later->setFocus();
        } else {
            auto* legacy = addAction(QStringLiteral("profileMigrationUseLegacy"), QStringLiteral("옛 프로필 사용"),
                                     ProfileMigrationChoice::UseLegacy);
            addAction(QStringLiteral("profileMigrationUseCurrent"), QStringLiteral("새 프로필 사용"),
                      ProfileMigrationChoice::UseCurrent);
            legacy->setFocus();
        }
        layout->addLayout(actions);
    }
}

ProfileMigrationChoice ProfileMigrationDialog::choice() const {
    return choice_;
}

void ProfileMigrationDialog::reject() {
    choice_ = ProfileMigrationChoice::Cancel;
    QDialog::reject();
}

} // namespace keefi
