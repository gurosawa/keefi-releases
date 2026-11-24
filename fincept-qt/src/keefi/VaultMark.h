#pragma once

#include <QElapsedTimer>
#include <QWidget>

#include <array>

class QVariantAnimation;

namespace keefi {

// The KEEFI wordmark on the vault gate. Each of its five letters stands for one
// new-password rule (K 12 characters, E upper case, E lower case, F digit,
// I symbol). It only learns which rules are met and whether the form beside it
// can be submitted; it never receives password text, and a fully lit mark does
// not mean a password is right.
class VaultMark final : public QWidget {
    Q_OBJECT
  public:
    enum class Letter { Off, On, Ready, Rejected };
    Q_ENUM(Letter)
    static constexpr int Letters = 5;
    static constexpr int RiseMs = 450;
    static constexpr int StaggerMs = 40;
    static constexpr int ShakeMs = 520;
    static constexpr qreal OffDrop = 6; // an unmet rule's letter sits this much lower

    explicit VaultMark(QWidget* parent = nullptr);
    QString text() const { return QStringLiteral("KEEFI"); }
    // Rule names under the letters, for creating a vault. Unlocking has one field
    // and no rules to show, so the letters light together.
    void setCaptionsVisible(bool visible);
    bool captionsVisible() const { return captions_; }
    // metRules: bit i is set while rule i is met. ready: the form can be submitted.
    void setProgress(int metRules, bool ready);
    // The password check blocks the UI thread right after this returns: finish
    // every movement and paint one still frame first.
    void hold();
    // A failed check. Lasts until the next input that meets anything.
    void reject();
    void reset();

    Letter letter(int index) const;
    qreal shownDrop(int index) const; // 0 at rest, OffDrop when unmet
    QColor shownColour(int index) const;
    qreal shake() const { return shakeX_; }
    bool settled() const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent*) override;
    void hideEvent(QHideEvent*) override;
    void changeEvent(QEvent*) override;

  private:
    struct Track {
        Letter to = Letter::Off;
        QColor fromColour;
        qreal fromDrop = OffDrop;
        qint64 startMs = 0;
        bool still = true; // shown at its target with no movement
    };
    void retarget(int index, Letter target, int delayMs);
    void kick();
    void settle();
    void describe();
    QString ruleName(int index) const;
    qreal eased(int index) const;
    QColor colour(Letter) const;
    static qreal drop(Letter letter) { return letter == Letter::Off ? OffDrop : 0; }
    std::array<Track, Letters> tracks_{};
    QElapsedTimer clock_;
    QVariantAnimation* ticker_ = nullptr;
    QVariantAnimation* shaker_ = nullptr;
    qreal shakeX_ = 0;
    bool captions_ = true;
    bool rejected_ = false;
    int met_ = 0;
    bool ready_ = false;
};

} // namespace keefi
