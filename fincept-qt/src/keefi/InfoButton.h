#pragma once
#include <QToolButton>
#include <memory>

namespace keefi {
struct PublicSourceInformation {
    QString basis, source, delay, unit, caution;
};

class InfoButton final : public QToolButton {
    Q_OBJECT
  public:
    explicit InfoButton(QWidget* parent = nullptr);
    ~InfoButton() override;
    void setInformation(const QString& title, const QString& publicText);
    void setPublicSourceInformation(const QString& title, const QString& summary,
        const PublicSourceInformation& fields, const QString& supplementalText = {});
    // Default false. The owning view must explicitly re-enable after its ready/unlock gate.
    void setPrivateAccess(bool allowed);
    bool setPrivateInformation(const QString& plainText);
    void clearPrivate(); // Revokes access; clears private widget/state synchronously and closes its dialog.
    bool informationOpen() const;
  public slots:
    void openInformation();
  signals:
    void informationOpened();
    void informationClosed();
  protected:
    void keyPressEvent(QKeyEvent*) override;
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}
