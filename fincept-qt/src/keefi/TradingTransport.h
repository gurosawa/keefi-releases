#pragma once
#include "keefi/TradingTypes.h"
#include <QObject>
#include <memory>
namespace keefi {
class TradingRequestLimits;
class TradingTransport : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;
    virtual void send(quint64 token, const RequestSpec& request) = 0;
    virtual void abort() = 0;
  signals:
    void finished(quint64 token, const keefi::HttpResult& result);
};
class NetworkTradingTransport final : public TradingTransport {
    Q_OBJECT
  public:
    explicit NetworkTradingTransport(QObject* parent = nullptr, std::shared_ptr<TradingRequestLimits> limits = {});
    ~NetworkTradingTransport() override;
    void send(quint64 token, const RequestSpec& request) override;
    void abort() override;
    static bool allowed(const RequestSpec& request);
    // Only an unsigned Binance exchangeInfo catalogue may exceed 4 MiB.
    static qsizetype responseBodyLimit(const RequestSpec& request);
  private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}
