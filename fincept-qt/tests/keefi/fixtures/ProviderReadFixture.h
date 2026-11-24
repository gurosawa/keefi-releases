#pragma once
#include "keefi/TradingTransport.h"
#include "TossResponses.h"
#include "KisResponses.h"
#include "KiwoomResponses.h"
#include "LsResponses.h"
#include "UpbitResponses.h"
#include "OkxResponses.h"
#include "BithumbResponses.h"
#include "BinanceUsdmResponses.h"
#include <QTest>
#include <QTimer>

namespace keefi::test {
struct ReadFixture {
    AccountRecord account;
    QMap<QString, QString> credentials;
    explicit ReadFixture(const QString& provider) {
        account.provider=provider=="binance-usdm"?"binance":provider;
        account.market=QStringList{"toss","kis","kiwoom","ls"}.contains(provider)?"domestic_equity":provider=="binance-usdm"?"usdm_futures":"spot";
        account.environment="live";account.alias="fake recovery";
        account.currency=account.market=="domestic_equity"||provider=="upbit"||provider=="bithumb"?"KRW":"USDT";
        credentials={{"api_key","FakeLifecycleKey123456"},{"secret","FakeLifecycleSecret123456"}};
        if(provider=="upbit")credentials={{"access_key","FakeLifecycleKey123456"},{"secret","FakeLifecycleSecret123456"}};
        else if(provider=="okx"){account.region="global";credentials["passphrase"]="Fake passphrase";}
        else if(provider=="toss")credentials={{"client_id","FakeLifecycleClient123456"},{"client_secret","FakeLifecycleSecret123456"}};
        else if(account.market=="domestic_equity"){
            credentials={{"app_key","FakeLifecycleKey123456"},{"app_secret","FakeLifecycleSecret123456"}};
            if(provider=="kis"){credentials["account_number"]="12345678";credentials["product_code"]="01";}
        }
    }
};
// Synthetic provider-shaped data from the existing, separately sourced adapter
// fixtures. The real adapter and production transport allowlist still run.
class ProviderReadTransport final : public TradingTransport {
public:
    QString provider;
    QList<Operation> sent;
    std::function<void(const RequestSpec&,HttpResult&)> transform;
    quint64 epoch=0;
    void abort() override { ++epoch; }
    void send(quint64 token,const RequestSpec& r) override {
        QVERIFY(!isMutation(r.operation));
        QVERIFY2(NetworkTradingTransport::allowed(r),qPrintable(provider+" operation "+QString::number(int(r.operation))));
        sent.append(r.operation);
        HttpResult response;const auto now=QDateTime::currentDateTimeUtc();
        if(provider=="binance-usdm")response=usdmResponse(r);
        else if(provider=="binance")response=usdmJson(r.operation==Operation::Time?QJsonObject{{"serverTime",QString::number(now.toMSecsSinceEpoch())}}:
            QJsonObject{{"canTrade",true},{"accountType","SPOT"},{"balances",QJsonArray{QJsonObject{{"asset","BTC"},{"free","0.125"},{"locked","0.025"}}}}});
        else if(provider=="upbit")response=upbit_fixture::response(r.operation);
        else if(provider=="okx")response=okxResponse(r);
        else if(provider=="bithumb")response=bithumbResponse(r);
        else if(provider=="toss")response=tossResponse(r.operation);
        else if(provider=="kis")response=kisResponse(r,now);
        else if(provider=="kiwoom")response=kiwoomResponse(r,now);
        else if(provider=="ls")response=lsResponse(r,now);
        if(transform)transform(r,response);
        const auto generation=epoch;
        QTimer::singleShot(0,this,[this,generation,token,response]{if(generation==epoch)emit finished(token,response);});
    }
};
}
