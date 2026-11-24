#pragma once
#include "keefi/SwapTypes.h"
#include <QObject>
#include <QNetworkRequest>
#include <QIODevice>
#include <memory>
namespace keefi {
class SolanaReadRpc final {
public:
    static SwapRequest request(SwapOperation,const QString& id,const QJsonArray& params,QString* error=nullptr);
    static SwapApiResult parse(SwapOperation,const HttpResult&,const QString& expectedId);
};
class SwapTransport : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void send(quint64,const SwapRequest&)=0;
    virtual void abort()=0;
signals:
    void finished(quint64 token,const keefi::HttpResult&);
};
class NetworkSwapTransport final : public SwapTransport {
    Q_OBJECT
public:
    explicit NetworkSwapTransport(QObject* parent=nullptr);
    ~NetworkSwapTransport() override;
    void send(quint64,const SwapRequest&) override;
    void abort() override;
    static bool allowed(const SwapRequest&);
    // Pure request policy used by send(); no request is issued by this helper.
    static QNetworkRequest requestPolicy(const SwapRequest&);
    // Consume one available response batch; false preserves the existing oversize outcome.
    static bool readResponseChunk(QIODevice&, QByteArray&);
private:
    struct Private; std::unique_ptr<Private> d;
};
}
