#pragma once
#include "keefi/BinanceBalanceStream.h"
#include <QJsonDocument>
#include <QJsonObject>

namespace keefi::test {
class BalanceSocket final : public BinanceBalanceSocket {
public:
    QUrl opened; QList<QByteArray> sent; int opens=0,aborts=0,pings=0;
    void open(const QUrl& url) override {opened=url;++opens;}
    void send(const QByteArray& bytes) override {sent.append(bytes);}
    void ping() override {++pings;}
    void abort() override {++aborts;}
    void deliver(const QJsonObject& object) {emit received(QJsonDocument(object).toJson(QJsonDocument::Compact));}
    QJsonValue lastId() const {return QJsonDocument::fromJson(sent.last()).object().value("id");}
    void time(qint64 at) {deliver({{"id",lastId()},{"status",200},{"result",QJsonObject{{"serverTime",at}}}});}
    void subscribed() {deliver({{"id",lastId()},{"status",200},{"result",QJsonObject{{"subscriptionId",7}}}});}
    void handshake(qint64 at) {emit connected();time(at);subscribed();}
    void event(qint64 at,QString type="outboundAccountPosition",int id=7) {
        deliver({{"subscriptionId",id},{"event",QJsonObject{{"e",type},{"E",at}}}});
    }
};
}
