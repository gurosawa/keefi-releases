#pragma once

#include <QPointer>
#include <QStringList>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QTableWidget;

namespace keefi {

// Never infer absence from a rounded display amount, a price failure or a net sum.
enum class AssetPresence { Held, NeedsCheck, Zero };
AssetPresence assetPresence(const QStringList& required, const QStringList& optional = {});
QList<int> assetDisplayOrder(const QList<AssetPresence>& presence);

class AssetListFilter final : public QWidget {
    Q_OBJECT
  public:
    explicit AssetListFilter(const QString& name, QWidget* parent = nullptr);
    struct Criteria {
        QString search;
        bool hideZero;
        bool matches(const QString& searchText, AssetPresence presence) const;
    };
    Criteria criteria() const;
    bool matches(const QString& searchText, AssetPresence presence) const;
    void setCounts(int visible, int held, int unknown, bool complete);
    // Metadata and hiding affect only this view; the caller retains every source row.
    static void annotate(QTableWidget*, int row, AssetPresence, const QString& searchText);
    void setTarget(QTableWidget*);
    void apply(bool complete);

  signals:
    void changed();

  private:
    QComboBox* scope_ = nullptr;
    QLineEdit* search_ = nullptr;
    QLabel* status_ = nullptr;
    QPointer<QTableWidget> target_;
    bool complete_ = false;
};
} // namespace keefi
