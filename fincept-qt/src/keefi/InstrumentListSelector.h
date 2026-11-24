#pragma once

#include "keefi/LocalComboBox.h"
#include <QPointer>

namespace keefi {

class InstrumentListSelectorView;

// The workspace supplies the projected rows and performs every saved-list
// mutation. The selector owns only the transient popup view and drag intent.
class InstrumentListSelector final : public LocalComboBox {
    Q_OBJECT
public:
    explicit InstrumentListSelector(QWidget* parent = nullptr);
    ~InstrumentListSelector() override;

    bool manualOrder() const;
    void setManualOrder(bool manual);
    void showPopup() override;
    void hidePopup() override;

signals:
    void orderViewChanged();
    void listMoveRequested(QString sourceId, QString targetId, bool after);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool manualOrder_ = false;
    QPointer<InstrumentListSelectorView> popupView_;
};

} // namespace keefi
