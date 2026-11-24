#pragma once

#include <QPointer>
#include <QToolButton>

class QLineEdit;

namespace keefi {

// Shows exactly one masked field only while the button is held. Leaving the
// button, losing focus, deactivation, hiding or disabling masks it again and a
// fresh press is required; Enter is consumed so it never submits the dialog.
class PressRevealButton final : public QToolButton {
    Q_OBJECT
  public:
    explicit PressRevealButton(QLineEdit* field, QWidget* parent = nullptr);
    ~PressRevealButton() override;
    bool revealing() const { return revealing_; }
    // Draw as a 28 px eye over the field's right edge. The button must be the
    // field's sibling (same parent), never its child: a child is deleted while
    // the field's QLineEdit part is already gone and would touch it.
    void setInlineEye();

  public slots:
    void conceal();

  protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void reveal();
    void place();
    bool inline_ = false;
    QPointer<QLineEdit> field_;
    QPointer<QWidget> window_;
    bool revealing_ = false;
    int heldKey_ = 0;
};

} // namespace keefi
