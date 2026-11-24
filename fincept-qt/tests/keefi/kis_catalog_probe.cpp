#include "keefi/KisMasterCatalog.h"
#include "keefi/TradingTransport.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>
#include <QTimer>

// Explicit developer probe: three unsigned public GETs, no credentials,
// personal profile, retry, order endpoint or mutation support.
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    keefi::NetworkTradingTransport transport;
    const QStringList markets{"KOSPI", "KOSDAQ", "KONEX"};
    int index = 0; bool passed = true; QJsonArray reports;
    auto send = [&] {
        keefi::RequestSpec request; request.operation = keefi::Operation::Markets;
        request.url = QUrl(keefi::KisMasterCatalog::sourceUrl(markets[index]));
        transport.send(quint64(index), request);
    };
    QObject::connect(&transport, &keefi::TradingTransport::finished, &app, [&](quint64 token, const keefi::HttpResult& response) {
        if (token != quint64(index)) { app.exit(2); return; }
        QString error;
        const auto items = response.status == 200 && !response.networkError && !response.timedOut
            ? keefi::KisMasterCatalog::parseZip(response.body, markets[index], &error) : QList<keefi::MarketInstrument>{};
        const bool ok = !items.isEmpty() && error.isEmpty(); passed = passed && ok;
        int eligible = 0; for (const auto& item : items) if (item.tradable) ++eligible;
        reports.append(QJsonObject{{"market", markets[index]}, {"source", keefi::KisMasterCatalog::sourceUrl(markets[index])},
            {"http_status", response.status}, {"network_error", response.networkError}, {"timed_out", response.timedOut},
            {"received_at_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"bytes", response.body.size()}, {"rows", items.size()}, {"catalogue_eligible", eligible}, {"parse_ok", ok}});
        if (++index == markets.size()) {
            QTextStream(stdout) << QJsonDocument(QJsonObject{{"public_only", true}, {"authenticated", false},
                {"account_verified", false}, {"all_passed", passed}, {"markets", reports}}).toJson();
            app.exit(passed ? 0 : 1);
        } else QTimer::singleShot(300, &app, send);
    });
    QTimer::singleShot(0, &app, send);
    return app.exec();
}
