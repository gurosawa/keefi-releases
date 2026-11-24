#pragma once
#include <QDialog>
#include <memory>

namespace keefi {
class Vault;
class PortfolioController;
class SwapJournal;
class RecordTransferDialog final : public QDialog {
    Q_OBJECT
  public:
    explicit RecordTransferDialog(Vault&, PortfolioController*, SwapJournal*, QWidget* parent=nullptr);
    ~RecordTransferDialog() override;
    void openRecords();
  protected:
    void done(int result) override;
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}
