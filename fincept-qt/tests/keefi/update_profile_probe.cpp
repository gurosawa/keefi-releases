#include "keefi/IdentityCompatibility.h"
#include "keefi/ProfileUpgrade.h"
#include "keefi/Vault.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>
#include <QSet>
using namespace keefi;

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 3 || !QDir::isAbsolutePath(args[2])) return 2;
    const bool seed = args[1] == QStringLiteral("seed");
    if (!seed && args[1] != QStringLiteral("check")) return 2;
    const QString folder = args[2];
    auto plan = planProfileUpgrade(std::optional<QString>{folder}, {});
    QString error;
    auto failed = [&] { QTextStream(stderr) << "Synthetic update fixture failed: " << error << Qt::endl; return 1; };
    if ((seed && plan.action != ProfileUpgradeAction::CreateCurrent) ||
        (!seed && plan.action != ProfileUpgradeAction::UseCurrent)) return 3;
    auto ready = seed ? createFreshProfile(plan, "0.18.0", &error) : existingProfileReady(plan, &error);
    if (ready.directory.isEmpty() || !configureIdentityScheme(ready.identityScheme, &error)) return failed();
    Vault vault(QDir(ready.directory).filePath("credentials.keefi-vault"), ready.identityScheme, nullptr);
    const QString password = QStringLiteral("Synthetic!UpdatePassword42");
    const QMap<QString, QMap<QString, QString>> credentials{
        {"kiwoom", {{"app_key", "synthetic-update-key"}, {"app_secret", "synthetic-update-secret"}}},
        {"toss", {{"client_id", "synthetic-update-id"}, {"client_secret", "synthetic-update-client-secret"}}}
    };
    if (seed) {
        if (!vault.create(password, &error)) return failed();
        for (auto it = credentials.begin(); it != credentials.end(); ++it) {
            AccountRecord account;
            account.provider = it.key();
            account.alias = "Synthetic update " + it.key();
            account.market = "domestic_equity";
            account.currency = "KRW";
            account.environment = "live";
            if (!vault.addAccount(account, it.value(), StorageMode::EncryptedVault, &error)) return failed();
        }
        vault.lock();
    }
    if (!vault.unlock(password, &error) || vault.accounts().size() != credentials.size()) return failed();
    QSet<QString> checked;
    for (const auto& account : vault.accounts()) {
        if (!credentials.contains(account.provider) || account.alias != "Synthetic update " + account.provider ||
            account.market != "domestic_equity" || account.currency != "KRW" || account.environment != "live") return 4;
        const auto expected = credentials.value(account.provider);
        bool equal = true;
        if (!vault.withCredentials(account.id, [&](const CredentialSet& saved) {
            equal = saved.size() == size_t(expected.size());
            for (auto it = expected.begin(); equal && it != expected.end(); ++it) {
                const auto actual = saved.find(it.key());
                equal = actual != saved.end() && QByteArrayView(reinterpret_cast<const char*>(actual->second.data()),
                    qsizetype(actual->second.size())) == it.value().toUtf8();
            }
        }, &error) || !equal) return failed();
        checked.insert(account.provider);
    }
    vault.lock();
    return checked.size() == credentials.size() ? 0 : 5;
}
