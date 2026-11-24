#pragma once
#include <QHash>
#include <QObject>
#include <QPointF>
#include <memory>

namespace keefi {
class TradingTransport;
struct RequestSpec;
struct HistoryTarget {
    QString section, id, sourceId; // Only coin history. CoinGecko unique coin id; empty uses Upbit KRW candles.
    QString key() const { return section + ":" + id; }
};
struct PublicHistory {
    QList<QPointF> points; // Actual source timestamps (UTC milliseconds), not evenly spaced invented values.
    qint64 receivedAtMs = 0;
    QString source, status;
};
// Keyless coin history only. Individual stock charts use a connected broker.
class PublicMarketHistory final : public QObject {
    Q_OBJECT
  public:
    explicit PublicMarketHistory(TradingTransport*, QObject* parent = nullptr);
    ~PublicMarketHistory() override;
    void setTargets(const QList<HistoryTarget>& targets);
    PublicHistory history(const QString& section, const QString& id) const;
    static RequestSpec request(const HistoryTarget& target);
    static bool isRequest(const RequestSpec& request);
    static bool parse(const HistoryTarget&, const QByteArray&, qint64 nowMs, PublicHistory*);
  signals:
    void changed();
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
} // namespace keefi
