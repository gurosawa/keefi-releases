#pragma once
#include "keefi/AssetsCapture.h"
#include <QDialog>
#include <memory>
namespace keefi {
class Vault;class TradingTransport;class AssetsLedger;class PortfolioController;
class AssetsWindow final : public QDialog {
    Q_OBJECT
public:
    explicit AssetsWindow(Vault&,QWidget* parent=nullptr,TradingTransport* prices=nullptr,AssetsLedger* shared=nullptr);
    ~AssetsWindow() override;
    void record(const AssetCapture&);
    void openRecords();
    void setCurrentPortfolio(PortfolioController*);
private:
    struct Private;std::unique_ptr<Private> d_;
};
}
