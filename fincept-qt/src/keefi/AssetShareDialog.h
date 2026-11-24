#pragma once
#include "keefi/AssetShareCard.h"
#include <QDialog>
#include <functional>
#include <memory>

namespace keefi {
// A frozen value snapshot. No network, profile, account or QWidget capture input.
class AssetShareDialog final : public QDialog {
    Q_OBJECT
  public:
    using PathChooser = std::function<QString(QWidget*)>;
    explicit AssetShareDialog(AssetShareSnapshot, AssetShareOptions = {}, QWidget* parent = nullptr);
    ~AssetShareDialog() override;
    AssetShareSnapshot snapshot() const;
    AssetShareOptions options() const;
    QImage previewImage() const;
    bool previewAvailable() const;
    void setPathChooser(PathChooser);
    void setSnapshot(AssetShareSnapshot); // Replaces a waiting snapshot; resets export consent.
    void setMarkets(const QList<WatchlistIdentity>& publicCandidates, const WatchlistIdentity& selected);
    void setPublicStatus(const QString& fixedPublicText); // Unavailable-preview UI only; never exported.
    void clearPersonalValues(); // Revokes pending chooser exports as well as the visible preview.
  signals:
    void saved(QString absoluteLocalPath);
    void saveFailed(QString publicReason);
    void marketRequested(keefi::WatchlistIdentity);
    void periodRequested(keefi::AssetSharePeriod);
  protected:
    void resizeEvent(QResizeEvent*) override;
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}
