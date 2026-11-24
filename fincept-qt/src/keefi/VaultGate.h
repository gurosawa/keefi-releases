#pragma once
#include <QDateTime>
#include <QPointer>
#include <QWidget>

class QLabel;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QFileDialog;

namespace keefi {
class VaultDialog;
class VaultMark;

// Startup presentation only. It receives file metadata, never vault contents.
// Password validation/reveal/clearing remain owned by the existing VaultDialog.
// One form at a time: unlock when this PC has a vault, otherwise create, and
// restore after a backup file is chosen.
class VaultGate final : public QWidget {
    Q_OBJECT
  public:
    explicit VaultGate(bool exists, const QDateTime& modifiedAt, QWidget* parent=nullptr);
    void refreshMetadata(bool exists, const QDateTime& modifiedAt);
    void setSecurityReady(bool ready);
    void clearSensitiveInputs();
    void resetPresentation();
    void setUpdateAvailable(bool available);
    void setOperationError(const QString& error);
  public slots:
    // Also the QFileDialog::fileSelected boundary; paths are never displayed.
    void selectBackup(const QString& path);
  signals:
    void browseRequested();
    void updateRequested();
    void submitted(int mode, const QString& currentPassword, const QString& newPassword, const QString& backupPath);
  protected:
    void hideEvent(QHideEvent*) override;
  private:
    void selectPage(bool existing);
    void showForm(VaultDialog* form);
    void refreshControls();
    void chooseBackup();
    bool exists_=false, ready_=false, busy_=false;
    QString backupPath_;
    QScrollArea* scroll_=nullptr;
    VaultMark* mark_=nullptr;
    QStackedWidget* forms_=nullptr;
    QLabel* title_=nullptr;
    QLabel* safety_=nullptr;
    QPushButton* backup_=nullptr;
    QPushButton* cancelRestore_=nullptr;
    QPushButton* browse_=nullptr;
    QPushButton* update_=nullptr;
    VaultDialog *create_=nullptr, *unlock_=nullptr, *restore_=nullptr;
    QPointer<QFileDialog> chooser_;
};
}
