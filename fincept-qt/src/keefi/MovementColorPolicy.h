#pragma once
#include "keefi/MovementDirection.h"
#include <QByteArray>
#include <QColor>
#include <QObject>
#include <memory>

namespace keefi {
enum class MovementScheme { GreenRed, KoreanRedBlue };
class MovementColorPolicy final : public QObject {
    Q_OBJECT
  public:
    static constexpr const char* ObjectName = "keefiMovementColorPolicy";
    static constexpr int MaxStateBytes = 1024;
    explicit MovementColorPolicy(QObject* parent = nullptr);
    ~MovementColorPolicy() override;
    MovementScheme scheme() const;
    void setScheme(MovementScheme);
    QColor foreground(MovementDirection) const;
    QColor fill(MovementDirection) const; // Original mockup ink for candle marks, not text.
    QColor heat(MovementDirection, int intensityBand) const; // Existing bands 0..2; no change calculation.
    static QString marker(MovementDirection); // Up ▲, Down ▼; Flat/Unknown empty.
    QByteArray publicState() const;
    bool restorePublicState(const QByteArray&, QString* error = nullptr);

    // Closest ancestor's direct policy child, or nullptr. No global mutable default.
    static MovementColorPolicy* forContext(const QObject*);
    static MovementScheme schemeFor(const QObject*);
    static QColor colorFor(const QObject*, MovementDirection);
    static QColor fillFor(const QObject*, MovementDirection);
    static QColor heatFor(const QObject*, MovementDirection, int intensityBand);
  signals:
    void changed();
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}
