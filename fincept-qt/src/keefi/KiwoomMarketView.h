#pragma once
#include <QWidget>
#include <memory>
namespace keefi {
class PortfolioController;
class KiwoomMarketStream;
// Read-only presentation/selection of an existing portfolio broker. Root owns
// the sole stream's broker/targets/country; this widget never starts a job.
class KiwoomMarketView final:public QWidget {
    Q_OBJECT
  public:
    KiwoomMarketView(PortfolioController*,KiwoomMarketStream*,QWidget* parent=nullptr);
    ~KiwoomMarketView() override;
    void setPrivateEnabled(bool);
    bool privateEnabled() const;
    void clearPrivate();
    QString selectedConnectionId() const;
    void selectConnection(const QString& id);
    void setActiveCountryDisplay(const QString&);
  signals:
    void selectedConnectionChanged();
    void activeCountryRequested(const QString&);
  protected:
    void showEvent(QShowEvent*) override;
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}
