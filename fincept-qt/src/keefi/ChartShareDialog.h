#pragma once
#include "keefi/ChartShareCard.h"
#include <QDialog>
#include <functional>
#include <memory>
namespace keefi {
// Value-only public snapshot preview. No live source, profile or screenshot input.
class ChartShareDialog final : public QDialog {
    Q_OBJECT
  public:
    using PathChooser=std::function<QString(QWidget*)>;
    explicit ChartShareDialog(ChartShareSnapshot,ChartShareOptions={},QWidget* parent=nullptr);
    ~ChartShareDialog() override;
    ChartShareSnapshot snapshot() const;
    ChartShareOptions options() const;
    QImage previewImage() const;
    bool previewAvailable() const;
    void setPathChooser(PathChooser); // Explicit test seam; empty restores the native chooser.
  signals:
    void saved(QString absoluteLocalPath);
    void saveFailed(QString publicReason);
  protected:
    void resizeEvent(QResizeEvent*) override;
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}
