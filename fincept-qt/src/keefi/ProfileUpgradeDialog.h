#pragma once

#include "ProfileUpgrade.h"
#include <QDialog>
#include <memory>

namespace keefi {

class ProfileUpgradeDialog final : public QDialog {
    Q_OBJECT
public:
    explicit ProfileUpgradeDialog(const ProfileUpgradePlan& plan, QWidget* parent = nullptr);
    ProfileUpgradeSourceKind sourceKind() const;
protected:
    void done(int result) override;
private:
    ProfileUpgradeSourceKind source_ = ProfileUpgradeSourceKind::None;
};

class ProfileUpgradePasswordDialog final : public QDialog {
    Q_OBJECT
public:
    ProfileUpgradePasswordDialog(const ProfileUpgradePlan& plan, const QString& version,
                                 ProfileStageConverter converter, QWidget* parent = nullptr);
    ~ProfileUpgradePasswordDialog() override;
    void setPrivateAccess(bool allowed);
    ProfileReady ready() const;
    QString error() const;
public slots:
    void lockAndClear(const QString& reason);
protected:
    void done(int result) override;
private:
    struct Private;
    std::unique_ptr<Private> d_;
};

} // namespace keefi
