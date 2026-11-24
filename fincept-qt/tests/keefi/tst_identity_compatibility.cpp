#include "keefi/IdentityCompatibility.h"
#include "keefi/OkxProtocol.h"
#include "keefi/TossAdapter.h"
#include "fixtures/TossResponses.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QtTest>

using namespace keefi;

namespace {
QString syntheticTossAccountScope() {
    TossAdapter adapter;
    if (!adapter.parse(Operation::Authenticate, test::tossResponse(Operation::Authenticate), {}).ok
        || !adapter.parse(Operation::Accounts, test::tossResponse(Operation::Accounts), {}).ok
        || adapter.accountChoices().size() != 1) return {};
    QString error;
    if (!adapter.selectBrokerAccount(adapter.accountChoices().first().handle, &error)) return {};
    return adapter.selectedAccountScope();
}
} // namespace

class IdentityCompatibilityTest : public QObject {
    Q_OBJECT
private slots:
    void currentProfileUsesCurrentDomains() {
        QCOMPARE(identitySchemeName(), QStringLiteral("keefi-v1"));
        QCOMPARE(identityDomain(IdentityDomain::EventReference), QByteArray("KEEFI/event-reference/v1"));
        QCOMPARE(okxAccountScope("global", "live", "123456"),
                 QStringLiteral("1e0ec82cccf4d9a8b5f847d7a89961012596519d6290c199191c775ffa5a54d8"));
    }

    void currentTossProfileUsesIndependentAccountScopeVector() {
        QCOMPARE(identitySchemeName(), QStringLiteral("keefi-v1"));
        // Independent Python hashlib/struct reference: the KEEFI domain, one
        // NUL, then five uint32-BE-length-prefixed synthetic fixture fields.
        // Evidence: checks/toss-identity-vector-reference.{py,json}.
        QCOMPARE(syntheticTossAccountScope(),
                 QStringLiteral("8c37c2b908ad1d4e1a0e408a956075f47a69d15f2a238bce76e25696a60ed581"));
    }

    void migratedProfilePreservesPublishedIdentityInSeparateProcess() {
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--probe-identity", "legacy-v1"});
        QVERIFY(child.waitForFinished(10000));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);
        const auto value = QJsonDocument::fromJson(child.readAllStandardOutput()).object();
        QCOMPARE(value.value("scheme").toString(), QStringLiteral("legacy-v1"));
        QCOMPARE(value.value("scope").toString(),
                 QStringLiteral("6178d15f7d6c5f0d575ba643a87e71d20add5336bd4385cb22047474a09ac2dd"));
        QCOMPARE(value.value("tossScope").toString(),
                 QStringLiteral("2c451bd01880ac145e05622de3a3afd8ef72fb08db8458c7deb341849075e5ea"));
        QCOMPARE(value.value("domainCount").toInt(), int(IdentityDomain::Count));
        QVERIFY(value.value("switchRejected").toBool());
        QVERIFY(value.value("invalidRejected").toBool());
        QCOMPARE(identitySchemeName(), QStringLiteral("keefi-v1"));
    }

    void invalidAndMidSessionChangesCannotChangeIdentity() {
        QString error;
        QVERIFY(configureIdentityScheme("keefi-v1", &error));
        QVERIFY(error.isEmpty());
        QVERIFY(!configureIdentityScheme("legacy-v1", &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!configureIdentityScheme("unknown", &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(identitySchemeName(), QStringLiteral("keefi-v1"));
        QCOMPARE(okxAccountScope("global", "live", "123456"),
                 QStringLiteral("1e0ec82cccf4d9a8b5f847d7a89961012596519d6290c199191c775ffa5a54d8"));
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 3 && args.at(1) == "--probe-identity") {
        QString error;
        if (!configureIdentityScheme(args.at(2), &error)) return 2;
        const auto initial = identitySchemeName();
        int count = 0;
        for (int i = 0; i < int(IdentityDomain::Count); ++i) {
            if (identityDomain(static_cast<IdentityDomain>(i)).isEmpty()) return 3;
            ++count;
        }
        const bool rejected = !configureIdentityScheme(initial == "legacy-v1" ? "keefi-v1" : "legacy-v1", &error);
        const bool invalid = !configureIdentityScheme("unsupported", &error);
        const auto tossScope = syntheticTossAccountScope();
        if (tossScope.isEmpty()) return 4;
        const QJsonObject result{{"scheme", identitySchemeName()},
                                 {"scope", okxAccountScope("global", "live", "123456")},
                                 {"tossScope", tossScope},
                                 {"domainCount", count}, {"switchRejected", rejected}, {"invalidRejected", invalid}};
        const auto output = QJsonDocument(result).toJson(QJsonDocument::Compact);
        fwrite(output.constData(), 1, std::size_t(output.size()), stdout);
        return 0;
    }
    IdentityCompatibilityTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_identity_compatibility.moc"
