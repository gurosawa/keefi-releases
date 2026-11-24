#pragma once

#include "keefi/ProfilePaths.h"
#include <QDialog>

namespace keefi {

class ProfileMigrationDialog final : public QDialog {
    Q_OBJECT
  public:
    explicit ProfileMigrationDialog(const ProfileMigrationPlan& plan, QWidget* parent = nullptr);
    ProfileMigrationChoice choice() const;
    void reject() override;
  private:
    ProfileMigrationChoice choice_ = ProfileMigrationChoice::Cancel;
};

} // namespace keefi
