#pragma once

#include <QDialog>
#include <QList>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;

namespace keefi {

class PressRevealButton;

class VaultDialog final : public QDialog {
    Q_OBJECT
  public:
    enum class Mode { Create, Unlock, ChangePassword };
    enum class Presentation { Dialog, Embedded };
    explicit VaultDialog(Mode mode, QWidget* parent = nullptr, Presentation presentation = Presentation::Dialog);
    ~VaultDialog() override;
    void setError(const QString& message);
    void clearSensitiveInputs();
    void resetForm();

  signals:
    void submitted(const QString& currentPassword, const QString& newPassword);
    // For an embedding screen: bit i set while new-password rule i is met
    // (12 characters, upper case, lower case, digit, symbol), and whether the
    // form can be submitted. An unlock form reports all five once its field has
    // input. Never password text or length.
    void ruleProgressChanged(int metRules, bool ready);

  protected:
    void done(int result) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void updateSubmitState();
    void setWorking(bool working);
    void showAsciiNotice();
    void fitContents();
    Mode mode_;
    Presentation presentation_;
    QLineEdit* current_ = nullptr;
    QLineEdit* password_ = nullptr;
    QLineEdit* confirm_ = nullptr;
    QList<PressRevealButton*> reveals_;
    QLabel* asciiNotice_ = nullptr;
    QLabel* error_ = nullptr;
    QLabel* progress_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QPushButton* submit_ = nullptr;
    QPushButton* cancel_ = nullptr;
    bool working_ = false;
    bool closing_ = false;
    int lastRules_ = -1;
    bool lastReady_ = false;
};

} // namespace keefi
