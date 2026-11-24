#pragma once
#include <QByteArray>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>
#include <optional>

namespace keefi {
enum class ChartPhraseKind { Pullback, ResultUp, ResultDown, ResultFlat, Notice, Line, Whale, Liquidation };
enum class ChartWhalePhraseSignal { None, LargeTrade, TakerSkew, OpenInterestJump, VolumeBurst, Rebound, TurnDown };
struct ChartPhraseFacts {
    QString symbol, source, currency, positionSide;
    qint64 intervalMs = 0, horizonMs = 0, observedAtMs = 0;
    int score = 0, samples = 0, noticeCount = 0;
    std::optional<double> resultReturnPercent, price, distancePercent;
    ChartWhalePhraseSignal whaleSignal = ChartWhalePhraseSignal::None;
    std::optional<QString> priceText; // HorizontalLine uses validated original decimal text.
};
struct ChartPhraseIssue { QString code, kind, phraseId, field; qsizetype offset = 0; };
struct ChartPhrasePreview { QString kind, phraseId, text; };
struct ChartPhraseValidation {
    bool valid = false;
    QByteArray sha256;
    QString packId, name;
    QMap<QString, int> counts;
    QList<ChartPhraseIssue> issues;
    QList<ChartPhrasePreview> preview; // Valid candidates only; never logged automatically.
};
struct ChartPhraseText {
    bool available = false, serious = false;
    QString title, body, phraseId, reason;
    QByteArray packSha256;
};

// Public templates only. No account, position amount, profile or clock access.
// A successful validation is not a human content review.
class ChartPhrasePack final : public QObject {
    Q_OBJECT
  public:
    using IndexSelector = std::function<int(int)>;
    explicit ChartPhrasePack(QObject* parent = nullptr);
    ~ChartPhrasePack() override;
    static ChartPhraseValidation validate(const QByteArray& utf8Json);
    static QByteArray plainJson();
    static QByteArray dripJson(); // Bundled drip pack used while no reviewed pack is imported.
    bool activateReviewed(const QByteArray& utf8Json, const QByteArray& exactReviewedSha256,
                          QString* error = nullptr);
    void clearImported();
    QByteArray importedSha256() const;
    ChartPhraseText render(ChartPhraseKind, const ChartPhraseFacts&, bool dripEnabled,
                           const QString& lastSuccessfulPhraseId = {}, IndexSelector = {}) const;
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
} // namespace keefi
