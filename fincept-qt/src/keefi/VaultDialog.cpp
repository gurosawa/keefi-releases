#include "keefi/VaultDialog.h"
#include "keefi/CompactControls.h"
#include "keefi/Vault.h"
#include "keefi/PressRevealButton.h"
#include "keefi/SecretInput.h"
#include "keefi/KeefiFonts.h"
#include "keefi/LedgerTextLabel.h"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QHash>
#include <QHideEvent>
#include <QShowEvent>
#include <QKeyEvent>
#include <QStyle>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QStringList>
#include <QVBoxLayout>
#include <QValidator>
#include <functional>

namespace keefi {
namespace {
// Modern Hangul syllables map to the keys of the Korean two-set (Dubeolsik)
// layout, not pronunciation. Already-ASCII bytes and their case stay exact.
// A committed syllable has no physical Shift/Caps history; doubled letters
// retain only the Shift explicitly represented by that Hangul character.
bool englishKeyboardText(const QString& input, QString* output, int* cursor = nullptr) {
    static const char* const initials[] = {"r","R","s","e","E","f","a","q","Q","t","T","d","w","W","c","z","x","v","g"};
    static const char* const vowels[] = {"k","o","i","O","j","p","u","P","h","hk","ho","hl","y","n","nj","np","nl","b","m","ml","l"};
    static const char* const finals[] = {"","r","R","rt","s","sw","sg","e","f","fr","fa","fq","ft","fx","fv","fg","a","q","qt","t","T","d","w","c","z","x","v","g"};
    static const char* const consonants[] = {"r","R","rt","s","sw","sg","e","E","f","fr","fa","fq","ft","fx","fv","fg","a","q","Q","qt","t","T","d","w","W","c","z","x","v","g"};
    QString result;
    int convertedCursor = 0;
    for (qsizetype index = 0; index < input.size(); ++index) {
        const auto character = input[index];
        const auto value = character.unicode();
        if (value >= 0x20 && value <= 0x7e) result += character;
        else if (value >= 0xac00 && value <= 0xd7a3) {
            const int syllable = value - 0xac00;
            result += QLatin1String(initials[syllable / (21 * 28)]);
            result += QLatin1String(vowels[(syllable / 28) % 21]);
            result += QLatin1String(finals[syllable % 28]);
        } else if (value >= 0x3131 && value <= 0x314e) result += QLatin1String(consonants[value - 0x3131]);
        else if (value >= 0x314f && value <= 0x3163) result += QLatin1String(vowels[value - 0x314f]);
        else if (value >= 0x1100 && value <= 0x1112) result += QLatin1String(initials[value - 0x1100]);
        else if (value >= 0x1161 && value <= 0x1175) result += QLatin1String(vowels[value - 0x1161]);
        else if (value >= 0x11a8 && value <= 0x11c2) result += QLatin1String(finals[value - 0x11a7]);
        else return false; // Never silently drop or partially accept unsupported characters.
        if (cursor && index < *cursor) convertedCursor = int(result.size());
    }
    if (cursor) *cursor = convertedCursor;
    *output = result;
    return true;
}

class AsciiPasswordValidator final : public QValidator {
  public:
    AsciiPasswordValidator(std::function<void()> rejected, QObject* parent)
        : QValidator(parent), rejected_(std::move(rejected)) {}
    State validate(QString& input, int& cursor) const override {
        if (isNewVaultPasswordText(input)) return Acceptable;
        QString converted;
        if (englishKeyboardText(input, &converted, &cursor)) {
            // Covers typing, paste, context-menu paste and drop through the same
            // QLineEdit boundary. Expanding a syllable also advances its cursor.
            // Qt clears edit history when a validator replaces text; never keep
            // the original Hangul secret as an undo alternative.
            input = converted;
            return Acceptable;
        }
        rejected_();
        return Invalid;
    }
  private:
    std::function<void()> rejected_;
};

// The US-layout character of a physical key, so typing in Korean input mode
// produces exactly the keys pressed (case from Shift/Caps Lock), never Hangul.
QChar physicalAscii(const QKeyEvent* event) {
    const bool shift = event->modifiers() & Qt::ShiftModifier;
#ifdef Q_OS_MACOS
    static const QHash<quint32, QPair<char, char>> keys{
        {0x00, {'a', 'A'}}, {0x01, {'s', 'S'}}, {0x02, {'d', 'D'}}, {0x03, {'f', 'F'}}, {0x04, {'h', 'H'}}, {0x05, {'g', 'G'}},
        {0x06, {'z', 'Z'}}, {0x07, {'x', 'X'}}, {0x08, {'c', 'C'}}, {0x09, {'v', 'V'}}, {0x0B, {'b', 'B'}}, {0x0C, {'q', 'Q'}},
        {0x0D, {'w', 'W'}}, {0x0E, {'e', 'E'}}, {0x0F, {'r', 'R'}}, {0x10, {'y', 'Y'}}, {0x11, {'t', 'T'}}, {0x12, {'1', '!'}},
        {0x13, {'2', '@'}}, {0x14, {'3', '#'}}, {0x15, {'4', '$'}}, {0x16, {'6', '^'}}, {0x17, {'5', '%'}}, {0x18, {'=', '+'}},
        {0x19, {'9', '('}}, {0x1A, {'7', '&'}}, {0x1B, {'-', '_'}}, {0x1C, {'8', '*'}}, {0x1D, {'0', ')'}}, {0x1E, {']', '}'}},
        {0x1F, {'o', 'O'}}, {0x20, {'u', 'U'}}, {0x21, {'[', '{'}}, {0x22, {'i', 'I'}}, {0x23, {'p', 'P'}}, {0x25, {'l', 'L'}},
        {0x26, {'j', 'J'}}, {0x27, {'\'', '"'}}, {0x28, {'k', 'K'}}, {0x29, {';', ':'}}, {0x2A, {'\\', '|'}}, {0x2B, {',', '<'}},
        {0x2C, {'/', '?'}}, {0x2D, {'n', 'N'}}, {0x2E, {'m', 'M'}}, {0x2F, {'.', '>'}}, {0x32, {'`', '~'}}, {0x31, {' ', ' '}}};
    const auto key = keys.find(event->nativeVirtualKey());
    if (key != keys.end()) {
        const bool caps = event->nativeModifiers() & (1u << 16); // NSEventModifierFlagCapsLock
        const char lower = key->first, upper = key->second;
        if (QChar(lower).isLetter()) return QChar((shift != caps) ? upper : lower);
        return QChar(shift ? upper : lower);
    }
#endif
    const int code = event->key();
    if (code >= Qt::Key_A && code <= Qt::Key_Z) return QChar((shift ? 'A' : 'a') + (code - Qt::Key_A));
    if (code >= Qt::Key_0 && code <= Qt::Key_9 && !shift) return QChar('0' + (code - Qt::Key_0));
    return {};
}

// A password field that never hosts an input-method composition while ASCII is
// enforced: the IME is switched off for it and Korean key text is replaced by
// the pressed key's US-layout character.
class AsciiPasswordEdit final : public QLineEdit {
  public:
    explicit AsciiPasswordEdit(QWidget* parent) : QLineEdit(parent) {}
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override {
        if (query == Qt::ImEnabled) return false;
        return QLineEdit::inputMethodQuery(query);
    }

  protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (!event->text().isEmpty() && !isNewVaultPasswordText(event->text()) &&
            !(event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))) {
            const QChar ascii = physicalAscii(event);
            if (!ascii.isNull()) {
                insert(QString(ascii));
                event->accept();
                return;
            }
        }
        QLineEdit::keyPressEvent(event);
    }
};

// An embedded field outlines itself in the danger colour after a failed
// operation, until something is typed into it again.
void markFieldError(QLineEdit* edit, bool error) {
    if (!edit || edit->property("error").toBool() == error) return;
    edit->setProperty("error", error);
    edit->style()->unpolish(edit);
    edit->style()->polish(edit);
}
} // namespace

VaultDialog::VaultDialog(Mode mode, QWidget* parent, Presentation presentation)
    : QDialog(parent), mode_(mode), presentation_(presentation) {
    setObjectName("vaultDialog");
    const bool embedded=presentation==Presentation::Embedded;
    if(embedded)setWindowFlags(Qt::Widget);
    setModal(!embedded);
    setMinimumWidth(embedded?0:440);
    setWindowTitle(mode == Mode::Create ? tr("금고 만들기")
                   : mode == Mode::Unlock ? tr("금고 잠금 해제") : tr("금고 암호 변경"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(embedded?0:20, embedded?0:18, embedded?0:20, embedded?0:18);
    layout->setSpacing(embedded ? 8 : 12);
    // The stacked create form is taller; its spare height must not spread the
    // unlock explanation and password controls apart.
    if(embedded)layout->setAlignment(Qt::AlignTop);
    auto* explanation = new QLabel(mode == Mode::Unlock
                                       ? tr("이 PC의 금융 인증정보를 사용하려면 금고 암호를 입력하세요.")
                                       : tr("이 암호는 이 PC의 금융 인증정보를 보호합니다. 금융기관 비밀번호와 다르게 설정하세요. 암호를 잃으면 저장한 키를 복구할 수 없습니다."), this);
    explanation->setObjectName("vaultExplanation");
    explanation->setWordWrap(true);
    // An embedded form sits under its own title; the explanation lives in the user guide.
    explanation->setVisible(!embedded);
    layout->addWidget(explanation);
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    if(embedded){form->setRowWrapPolicy(QFormLayout::WrapAllRows);form->setVerticalSpacing(8);}
    layout->addLayout(form);
    auto addPassword = [this, form](const QString& label, const QString& shortLabel, const char* name, bool newPassword) {
        auto* edit = new AsciiPasswordEdit(this);
        edit->setObjectName(name);
        edit->setEchoMode(QLineEdit::Password);
        edit->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase | Qt::ImhLatinOnly);
        // An over-limit value stays invalid instead of becoming a valid prefix.
        edit->setMaxLength(int(Vault::MaxPasswordBytes) + 1);
        // Validation errors must never compress the native text editor.
        if (presentation_ == Presentation::Embedded) applyCompactInput(edit);
        else edit->setMinimumHeight(qMax(44, edit->sizeHint().height()));
        if (newPassword)
            edit->setPlaceholderText(tr("영문 대문자·소문자·숫자·특수문자를 포함해 12자 이상 입력하세요."));
        edit->setValidator(new AsciiPasswordValidator([this] { showAsciiNotice(); }, edit));
        // IME composition never reaches the field: Hangul preedit text would
        // otherwise be shown or committed before the validator sees it.
        edit->installEventFilter(this);
        connect(edit, &QLineEdit::textChanged, this, &VaultDialog::updateSubmitState);
        if (presentation_ == Presentation::Embedded) {
            // One quiet field: its name is the placeholder, and the reveal
            // control sits over the field's right edge. It is the field's
            // sibling, not its child: a child would outlive the QLineEdit part
            // of its field while being deleted and touch it from its destructor.
            edit->setPlaceholderText(shortLabel);
            edit->setAccessibleName(shortLabel);
            auto* row = new QWidget(this);
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->addWidget(edit, 1);
            auto* reveal = new PressRevealButton(edit, row);
            reveal->setObjectName(QByteArray(name) + "Reveal");
            reveal->setProperty("secretReveal", true);
            reveal->setInlineEye();
            reveals_.append(reveal);
            form->addRow(row);
            return edit;
        }
        auto* row = new QWidget(this);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->addWidget(edit, 1);
        auto* reveal = new PressRevealButton(edit, row);
        reveal->setObjectName(QByteArray(name) + "Reveal");
        reveal->setMinimumSize(56, 44);
        reveal->setProperty("secretReveal", true);
        rowLayout->addWidget(reveal);
        reveals_.append(reveal);
        form->addRow(label, row);
        return edit;
    };
    if (mode != Mode::Create) {
        const QString name = mode == Mode::Unlock ? tr("금고 암호") : tr("현재 암호");
        current_ = addPassword(name, name, "currentPassword", false);
    }
    if (mode != Mode::Unlock) {
        password_ = addPassword(tr("새 암호 (12자 이상)"), tr("새 암호"), "newPassword", true);
        confirm_ = addPassword(tr("새 암호 확인"), tr("암호 확인"), "confirmPassword", true);
        auto* requirements = new LedgerTextLabel(tr(
            "12자 이상 · 영문 대문자·소문자·숫자·특수문자 포함\n"
            "공백은 특수문자로 세지 않습니다."), FontRole::Secondary14, this);
        requirements->setObjectName("passwordRequirements");
        requirements->setTextFormat(Qt::PlainText);
        requirements->setWordWrap(true);
        // Embedded, the wordmark above the form lights one letter per rule.
        requirements->setVisible(!embedded);
        if (embedded) password_->setAccessibleDescription(requirements->text());
        layout->addWidget(requirements);
    }
    asciiNotice_ = new QLabel(this);
    asciiNotice_->setObjectName("passwordInputNotice");
    asciiNotice_->setTextFormat(Qt::PlainText);
    asciiNotice_->setProperty("tone", "error");
    asciiNotice_->setWordWrap(true);
    asciiNotice_->hide();
    layout->addWidget(asciiNotice_);
    error_ = new QLabel(this);
    error_->setObjectName("vaultError");
    error_->setTextFormat(Qt::PlainText);
    error_->setProperty("tone", "error");
    error_->setWordWrap(true);
    // Embedded, one line is always reserved so a message never moves the button.
    if (embedded) error_->setMinimumHeight(22);
    error_->setVisible(embedded);
    layout->addWidget(error_);
    progress_ = new QLabel(this);
    progress_->setObjectName("vaultProgress");
    progress_->setTextFormat(Qt::PlainText);
    progress_->setWordWrap(true);
    progress_->hide();
    layout->addWidget(progress_);
    progressBar_ = new QProgressBar(this);
    progressBar_->setObjectName("vaultProgressBar");
    progressBar_->setAccessibleName(tr("금고 처리 진행 중"));
    progressBar_->setRange(0, 0);
    progressBar_->setTextVisible(false);
    progressBar_->hide();
    layout->addWidget(progressBar_);
    auto* buttons = new QDialogButtonBox(this);
    submit_ = buttons->addButton(mode == Mode::Create ? tr("금고 만들기")
                                : mode == Mode::Unlock ? tr("잠금 해제") : tr("암호 변경"), QDialogButtonBox::AcceptRole);
    submit_->setObjectName("submitVault");
    submit_->setProperty("primary", true);
    cancel_ = buttons->addButton(tr("취소"), QDialogButtonBox::RejectRole);
    cancel_->setObjectName("cancelVault");
    submit_->setMinimumSize(44, 44);
    cancel_->setMinimumSize(44, 44);
    if(embedded){
        buttons->removeButton(submit_);cancel_->hide();buttons->hide();
        applyCompactButton(submit_);
        submit_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
        layout->addWidget(submit_);
        if (mode == Mode::Create) {
            auto* loss = new QLabel(tr("암호를 잊으면 저장한 키를 되찾을 수 없어요."), this);
            loss->setObjectName("vaultLossWarning");
            loss->setTextFormat(Qt::PlainText);
            loss->setWordWrap(true);
            loss->setAlignment(Qt::AlignHCenter);
            loss->setFont(KeefiFonts::font(FontRole::Caption12));
            setThemeStyle(loss, QStringLiteral("color:${text3};"));
            layout->addSpacing(4);
            layout->addWidget(loss);
        }
    }else layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    const auto submitOperation=[this] {
        if (working_ || !submit_->isEnabled())
            return;
        const QString current = current_ ? current_->text() : QString();
        const QString next = password_ ? password_->text() : QString();
        setWorking(true);
        emit submitted(current, next);
    };
    if(embedded)connect(submit_,&QPushButton::clicked,this,submitOperation);
    else connect(buttons,&QDialogButtonBox::accepted,this,submitOperation);
    if(!embedded)resize(480, sizeHint().height());
    updateSubmitState();
}

VaultDialog::~VaultDialog() { clearSensitiveInputs(); }

void VaultDialog::resetForm() {
    closing_=false;
    setWorking(false);
    clearSensitiveInputs();
    error_->clear();error_->setVisible(presentation_==Presentation::Embedded);
    for (auto* edit : {current_, password_, confirm_}) markFieldError(edit, false);
}

void VaultDialog::hideEvent(QHideEvent* event) {
    if(presentation_==Presentation::Embedded)resetForm();
    QDialog::hideEvent(event);
}

void VaultDialog::showEvent(QShowEvent* event) {
    closing_=false;
    QDialog::showEvent(event);
    fitContents();
}

void VaultDialog::setError(const QString& message) {
    setWorking(false);
    clearSensitiveInputs();
    // Clearing inputs emits textChanged; preserve the operation error after
    // local validation has run. Editing again shows current input conditions.
    error_->setText(message);
    error_->setVisible(presentation_ == Presentation::Embedded || !message.isEmpty());
    if (presentation_ == Presentation::Embedded)
        for (auto* edit : {current_, password_, confirm_}) markFieldError(edit, !message.isEmpty());
    fitContents();
    if (current_) current_->setFocus();
    else if (password_) password_->setFocus();
}

void VaultDialog::setWorking(bool working) {
    working_ = working;
    if (working) clearSensitiveInputs();
    for (auto* edit : {current_, password_, confirm_})
        if (edit) edit->setEnabled(!working);
    for (auto* reveal : reveals_) reveal->setEnabled(!working);
    cancel_->setEnabled(!working);
    submit_->setText(working ? tr("처리 중…")
        : mode_ == Mode::Create ? tr("금고 만들기") : mode_ == Mode::Unlock ? tr("잠금 해제") : tr("암호 변경"));
    progress_->setText(mode_ == Mode::Create ? tr("금고를 만드는 중이에요…")
        : mode_ == Mode::Unlock ? tr("금고 암호를 확인하고 있어요…") : tr("금고 암호를 변경하고 있어요…"));
    // Embedded, the button's own text and the wordmark carry the still frame.
    progress_->setVisible(working && presentation_ == Presentation::Dialog);
    progressBar_->setVisible(working && presentation_ == Presentation::Dialog);
    updateSubmitState();
    if (working) {
        // Vault owns this Qt thread synchronously. Paint before its password KDF
        // starts, without pumping unrelated input or security events mid-call.
        layout()->activate();
        repaint();
        progress_->repaint();
        progressBar_->repaint();
    }
}

void VaultDialog::clearSensitiveInputs() {
    for (auto* reveal : reveals_)
        reveal->conceal();
    if (asciiNotice_)
        asciiNotice_->hide();
    for (auto* edit : {current_, password_, confirm_}) {
        if (edit) {
            edit->setEchoMode(QLineEdit::Password);
            edit->setText(QString()); // Also discard undo/redo history containing prior input.
        }
    }
    fitContents();
}

void VaultDialog::done(int result) {
    // Clear/conceal secrets synchronously before finished(), without resizing
    // a native window that is about to close. Embedded forms stay in place.
    closing_=presentation_==Presentation::Dialog;
    setWorking(false);
    clearSensitiveInputs();
    if(presentation_==Presentation::Embedded)return;
    QDialog::done(result);
}

void VaultDialog::showAsciiNotice() {
    if (!asciiNotice_)
        return;
    asciiNotice_->setProperty("tone", "error");
    asciiNotice_->style()->unpolish(asciiNotice_);
    asciiNotice_->style()->polish(asciiNotice_);
    asciiNotice_->setText(tr("지원하지 않는 문자가 있어 입력하지 않았습니다."));
    asciiNotice_->show();
    fitContents();
}

void VaultDialog::fitContents() {
    if (closing_ || !layout()) return;
    ensurePolished();
    if(presentation_==Presentation::Embedded){layout()->invalidate();updateGeometry();return;}
    layout()->activate();
    const int height = layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(width()) : sizeHint().height();
    if (height > 0) resize(width(), height);
}

bool VaultDialog::eventFilter(QObject* watched, QEvent* event) {
    const bool passwordField = watched == password_ || watched == confirm_ || watched == current_;
    if (presentation_ == Presentation::Embedded && passwordField && event->type() == QEvent::KeyPress) {
        const auto key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Escape) {
            resetForm();
            return true;
        }
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            // The embedded primary button sits outside the dialog button box.
            submit_->click();
            return true;
        }
    }
    if (event->type() == QEvent::InputMethod && watched && passwordField) {
        const auto* input = static_cast<QInputMethodEvent*>(event);
        QString converted;
        if (!englishKeyboardText(input->commitString(), &converted)) {
            showAsciiNotice(); return true;
        }
        const bool changed = converted != input->commitString();
        if (changed || !input->preeditString().isEmpty()) {
            // Strip transient preedit; commit once through QLineEdit so selection
            // and IME replacement offsets keep their documented semantics.
            QInputMethodEvent commit;
            commit.setCommitString(converted, input->replacementStart(), input->replacementLength());
            QCoreApplication::sendEvent(watched, &commit);
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void VaultDialog::updateSubmitState() {
    if (!submit_)
        return;
    // Rejected edits leave the text unchanged, so any accepted edit clears the notice.
    bool allAscii = true;
    for (auto* edit : {password_, confirm_})
        allAscii = allAscii && (!edit || isNewVaultPasswordText(edit->text()));
    if (asciiNotice_ && allAscii && (!current_ || isNewVaultPasswordText(current_->text())))
        asciiNotice_->hide();
    const bool currentReady = !current_ || (!current_->text().isEmpty() && isNewVaultPasswordText(current_->text()));
    QString passwordError;
    const bool newValid = !password_ || Vault::validateNewPassword(password_->text(), &passwordError);
    const bool matches = !password_ || password_->text() == confirm_->text();
    const bool newReady = newValid && matches;
    bool oversized = false;
    for (auto* edit : {current_, password_, confirm_})
        if (edit)
            oversized = oversized || exceedsSecretByteLimit(edit->text(), Vault::MaxPasswordBytes);
    if (oversized) {
        error_->setText(tr("금고 암호는 UTF-8 1024바이트 이하여야 합니다. 너무 긴 입력은 사용할 수 없습니다."));
    } else if (presentation_ == Presentation::Embedded) {
        bool typed = false;
        for (auto* edit : {current_, password_, confirm_}) typed = typed || (edit && !edit->text().isEmpty());
        if (typed) for (auto* edit : {current_, password_, confirm_}) markFieldError(edit, false);
        // The wordmark shows which rules are met; only a mismatch needs words.
        error_->setText(password_ && !confirm_->text().isEmpty() && !matches ? tr("두 암호가 달라요.") : QString());
    } else {
        QStringList messages;
        if (password_) {
            if (!newValid && (!password_->text().isEmpty() || !confirm_->text().isEmpty()))
                messages.append(passwordError);
            if (!confirm_->text().isEmpty() && !matches)
                messages.append(tr("새 암호와 암호 확인이 일치하지 않습니다."));
            else if (newValid && confirm_->text().isEmpty())
                messages.append(tr("새 암호 확인을 입력해 주세요."));
            if (newReady && !currentReady)
                messages.append(tr("현재 암호를 입력해 주세요."));
        }
        error_->setText(messages.join(QLatin1Char('\n')));
    }
    error_->setVisible(presentation_ == Presentation::Embedded || !error_->text().isEmpty());
    const bool ready = !working_ && currentReady && newReady && !oversized;
    submit_->setEnabled(ready);
    fitContents();
    // Starting work clears the fields; the embedding screen keeps its still
    // frame until the operation returns.
    if (working_) return;
    int rules = 0;
    if (password_) {
        // The same five character rules as Vault::validateNewPassword, for display only.
        bool upper = false, lower = false, digit = false, symbol = false;
        const auto text = password_->text();
        for (const QChar character : text) {
            const auto value = character.unicode();
            upper = upper || (value >= 'A' && value <= 'Z');
            lower = lower || (value >= 'a' && value <= 'z');
            digit = digit || (value >= '0' && value <= '9');
            symbol = symbol || (value >= 0x21 && value <= 0x2f) || (value >= 0x3a && value <= 0x40) ||
                (value >= 0x5b && value <= 0x60) || (value >= 0x7b && value <= 0x7e);
        }
        rules = int(text.size() >= Vault::MinPasswordCharacters) | int(upper) << 1 | int(lower) << 2 | int(digit) << 3 | int(symbol) << 4;
    } else if (currentReady) {
        rules = 0b11111;
    }
    if (rules == lastRules_ && ready == lastReady_) return;
    lastRules_ = rules;
    lastReady_ = ready;
    emit ruleProgressChanged(rules, ready);
}

} // namespace keefi
