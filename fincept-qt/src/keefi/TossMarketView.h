#pragma once
#include <QPointer>
#include <QStringList>
#include <QWidget>
class QComboBox;
class QLabel;
namespace keefi {
class PortfolioController;
class BrokerTradingController;
class TossMarketStream;
// Uses an already authenticated portfolio session; never starts authentication.
class TossMarketView final : public QWidget {
    Q_OBJECT
  public:
    TossMarketView(PortfolioController*, TossMarketStream*, QWidget* parent = nullptr);
    ~TossMarketView() override;
    void setSelection(const QString&);
    void setWatchlist(const QStringList&);
    void setMapSymbols(const QStringList&);
    // Full stock:KR/US:code identities; never infer a chart identity's country from its code.
    void setChartContext(bool active,const QStringList& visible,const QString& selected,const QStringList& remaining);
    QString selectedConnectionId() const;
    void selectConnection(const QString& id);
  signals:
    void selectedConnectionChanged();
  protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
  private:
    void synchronize();
    void clearPrivate();
    void refresh();
    QPointer<PortfolioController> portfolio_;
    QPointer<TossMarketStream> stream_;
    QPointer<BrokerTradingController> broker_;
    QString selection_, selectedConnection_, excludedReason_;
    QStringList watchlist_, mapSymbols_, chartVisible_, chartRemaining_;
    QString chartSelection_;
    bool chartActive_ = false;
    bool synchronizing_ = false;
    QComboBox* connections_ = nullptr;
    QLabel *account_ = nullptr, *status_ = nullptr, *quotes_ = nullptr, *evidence_ = nullptr, *excluded_ = nullptr;
};
}
