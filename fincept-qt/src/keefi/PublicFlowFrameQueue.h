#pragma once
#include <QByteArray>
#include <QList>
#include <QJsonObject>
#include "keefi/TradingTypes.h"
namespace keefi {
// Public wire frames only. The protocol still validates unchanged source times.
class PublicFlowFrameQueue {
public:
    bool push(const QByteArray& frame, qint64 now, qint64 elapsed) {
        if(frame.size()>65536 || frames_.size()>=128 || bytes_+frame.size()>2*1024*1024)return false;
        auto object=parseExactJson(frame).object();
        if(object.value("data").isObject())object=object.value("data").toObject();
        qint64 source=0;
        const auto include=[&](const QJsonValue& value){
            const auto text=value.toString();bool ok=false;
            for(const auto ch:text)if(ch<'0'||ch>'9')return;
            const auto time=text.toLongLong(&ok);if(ok && time>source)source=time;
        };
        for(const auto* key:{"E","T","timestamp","trade_timestamp"})include(object.value(key));
        include(object.value("o").toObject().value("T"));
        // Only a short clock lead is held. A larger/malformed timestamp still
        // reaches the strict parser unchanged and is rejected there.
        const auto release=source>now && source-now<=2000 ? source : now;
        frames_.append({frame,release,elapsed});bytes_+=frame.size();return true;
    }
    QList<QByteArray> takeReady(qint64 now, qint64 elapsed) {
        QList<QByteArray> out;
        while(!frames_.isEmpty() && waitMs(now,elapsed)==0){
            auto first=frames_.takeFirst();bytes_-=first.frame.size();out.append(std::move(first.frame));
        }
        return out;
    }
    int waitMs(qint64 now, qint64 elapsed) const {
        if(frames_.isEmpty())return -1;
        const auto& first=frames_.first();
        // Monotonic expiry bounds retention even if the local wall clock moves
        // backwards. Expiry never makes a future timestamp valid.
        return int(qMax(qint64(0),qMin(first.releaseAt-now,qint64(2000)-(elapsed-first.enqueued))));
    }
    void clear() { frames_.clear();bytes_=0; }
private:
    struct Frame { QByteArray frame; qint64 releaseAt; qint64 enqueued; };
    QList<Frame> frames_;qsizetype bytes_=0;
};
}
