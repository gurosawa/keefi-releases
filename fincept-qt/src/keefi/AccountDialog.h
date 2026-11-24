#pragma once

#include <QDialog>
#include <QMap>
#include <QStringList>
#include <QPointer>
#include <array>
#include "keefi/AccountModel.h"

class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QVBoxLayout;

namespace keefi {

class PressRevealButton;
class PortfolioController;

struct AccountDraft {
    QString provider;
    QString alias;
    QString environment;
    QString market;
    QString currency;
    QString region;
    QString sourceAccountId;
    QMap<QString, QString> credentials;
};

class AccountDialog final : public QDialog {
    Q_OBJECT
  public:
    explicit AccountDialog(bool persistentAvailable, QWidget* parent = nullptr);
    AccountDialog(bool persistentAvailable, QWidget* parent, bool stockChartOrigin);
    ~AccountDialog() override;
    void setReplacement(const AccountDraft& account);
    void setCredentialSources(const QList<AccountRecord>& accounts);
    void setSharedCredentialUsers(const QList<AccountRecord>& accounts);
    void setError(const QString& message);
    void clearSensitiveInputs();
    void setPersistentAvailable(bool available);
    void showConnectionResult(const QString& connectionId, PortfolioController*);
    bool importKiwoomKeyFiles(const QStringList& paths);

  signals:
    void submitted(const keefi::AccountDraft& draft);
    void vaultRequested();
    void connectionRequested(const QString&);
    void assetsRequested(const QString&);
    void chartReturnRequested(const QString& connectionId);
    void existingChartConnectionRequested(const QString& connectionId);

  protected:
    void done(int result) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void rebuildProvider();
    void refreshGuide();
    void updateSubmitState();
    void updateEnvironments();
    void updateMarketForm();
    bool updatePhantomAddressFeedback();
    QString okxRegionForSubmission() const;
    void updateCredentialSources();
    void updateCredentialSourceSelection();
    void normalizeCredential(QLineEdit* edit);
    bool pasteTrimmed(QLineEdit* edit);
    void showStep(int step);
    void chooseService(const QString& id);
    void refreshConnectionResult();
    void fitConnectionResult();
    void returnToChart();
    QStackedWidget* steps_ = nullptr;
    QVBoxLayout* existingChartAccounts_ = nullptr;
    std::array<QLabel*, 3> stepLabels_{};
    QLabel* selectedService_ = nullptr;
    QLabel* result_ = nullptr;
    QScrollArea* resultScroll_ = nullptr;
    QPushButton* back_ = nullptr;
    QPushButton* finish_ = nullptr;
    QPushButton* retry_ = nullptr;
    QPointer<PortfolioController> resultPortfolio_;
    QString resultConnectionId_;
    QComboBox* provider_ = nullptr;
    QLineEdit* alias_ = nullptr;
    QComboBox* environment_ = nullptr;
    QComboBox* region_ = nullptr;
    QLabel* regionLabel_ = nullptr;
    QComboBox* market_ = nullptr;
    QComboBox* currency_ = nullptr;
    QComboBox* credentialSource_ = nullptr;
    QLabel* credentialSourceLabel_ = nullptr;
    QLabel* sharedCredentialHint_ = nullptr;
    QLabel* storageHint_ = nullptr;
    QPushButton* openVault_ = nullptr;
    QList<AccountRecord> credentialSources_;
    QWidget* credentialsWidget_ = nullptr;
    QFormLayout* credentialsForm_ = nullptr;
    QLabel* addressFeedback_ = nullptr;
    QPushButton* switchAddressMarket_ = nullptr;
    QMap<QString, QLineEdit*> inputs_;
    QStringList required_;
    QList<PressRevealButton*> reveals_;
    QLabel* trimNotice_ = nullptr;
    QLabel* guide_ = nullptr;
    QPushButton* issueKey_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* error_ = nullptr;
    QPushButton* submit_ = nullptr;
    bool persistentAvailable_ = false;
    bool replacing_ = false;
    bool stockChartOrigin_ = false;
    bool chartReturnQueued_ = false;
    bool chartReturnSent_ = false;
    QString replacementRegion_;
    bool resultHasAssets_ = false;
};

} // namespace keefi

Q_DECLARE_METATYPE(keefi::AccountDraft)
