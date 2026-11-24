// Synthetic-only legacy artifact generator. Link the recorded Dev99 library;
// never rebuild Vault.cpp from the candidate source to create this fixture.
#include "finix/Vault.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {
const QString Password = QStringLiteral("FINIX synthetic Dev99 password 2026!");
const QString AccountId = QStringLiteral("00000000-0000-4000-8000-000000000099");
const QString Alias = QStringLiteral("DEV99_SYNTHETIC_ACCOUNT_NOT_REAL");
const QString Access = QStringLiteral("FINIX_DEV99_FAKE_ACCESS_KEY_NOT_REAL");
const QString Secret = QStringLiteral("FINIX_DEV99_FAKE_SECRET_NOT_REAL");
const QByteArray LibraryHash("5fc35dac2224dd99e03e7831c727852b56ea1c14862a994d9e68f824c1a08340");
const QByteArray SourceHash("f4eb62c8108633e80415e9dd0675bfc7abbeaff9a3c794e34478c63effe8530e");

QByteArray read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
QByteArray sha256(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}
bool writeNew(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        && file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
        && file.write(bytes) == bytes.size() && file.flush();
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("FINIX");
    QCoreApplication::setApplicationName("FINIX Dev");
    if (app.arguments().size() != 5) return 2;
    const QDir output(QFileInfo(app.arguments().at(1)).absoluteFilePath());
    if (!output.absolutePath().endsWith("/tests/finix/fixtures/dev99-profile")) return 3;
    const QString vaultOutput = output.filePath("credentials.finix-vault");
    const QString manifestOutput = output.filePath("manifest.json");
    if (QFileInfo::exists(vaultOutput) || QFileInfo::exists(manifestOutput)) return 4;
    if (sha256(read(app.arguments().at(2))) != LibraryHash
        || sha256(read(app.arguments().at(3))) != SourceHash) return 5;
    if (app.arguments().at(4) != "cef32c29c709b0f127c4fa118ef7e9617dcd5d05") return 6;

    QTemporaryDir temporary;
    if (!temporary.isValid()) return 7;
    const auto source = temporary.filePath("credentials.finix-vault");
    finix::Vault vault(source);
    QString error;
    if (!vault.create(Password, &error)) { qCritical().noquote() << error; return 8; }
    finix::AccountRecord record;
    record.id = AccountId;
    record.provider = "upbit";
    record.alias = Alias;
    record.environment = "live";
    record.market = "spot";
    record.currency = "KRW";
    if (!vault.addAccount(record, {{"access_key", Access}, {"secret", Secret}},
                          finix::StorageMode::EncryptedVault, &error)) {
        qCritical().noquote() << error; return 9;
    }
    bool ledgerKeyReady = false;
    if (!vault.withAssetsLedgerKey(true, [&](const finix::SecureBuffer& key) {
            ledgerKeyReady = key.size() == 32;
        }, &error) || !ledgerKeyReady) return 10;
    vault.lock();
    if (vault.isUnlocked() || !vault.accounts().isEmpty()) return 11;
    const auto original = read(source);
    if (!original.startsWith("FINIXVLT") || original.contains(Access.toUtf8())
        || original.contains(Secret.toUtf8()) || original.contains(Alias.toUtf8())) return 12;

    // The old implementation must authenticate its finished file without any
    // rewrite. The candidate will independently repeat this on a private copy.
    if (!vault.unlock(Password, &error) || vault.accounts().size() != 1
        || vault.accounts().first().id != AccountId || vault.accounts().first().alias != Alias
        || !vault.hasAssetsLedgerKey()) return 13;
    vault.lock();
    if (read(source) != original) return 14;
    if (!QDir().mkpath(output.absolutePath()) || !writeNew(vaultOutput, original)) return 15;

    QJsonObject manifest{
        {"artifact", "Dev99 synthetic legacy vault"},
        {"generatedUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"sourceCommit", app.arguments().at(4)},
        {"oldLibrary", "libfinix_vault.a"},
        {"oldLibrarySha256", QString::fromLatin1(LibraryHash)},
        {"oldVaultSourceSha256", QString::fromLatin1(SourceHash)},
        {"generatorCompiler", QString::fromLatin1(__clang_version__)},
        {"qtVersion", QString::fromLatin1(qVersion())},
        {"file", "credentials.finix-vault"},
        {"bytes", qint64(original.size())},
        {"sha256", QString::fromLatin1(sha256(original))},
        {"syntheticPassword", Password},
        {"syntheticAccountId", AccountId},
        {"syntheticAlias", Alias},
        {"provider", "upbit"},
        {"environment", "live"},
        {"market", "spot"},
        {"currency", "KRW"},
        {"hasLedgerKey", true},
        {"sourceUnchangedAfterReopen", true},
        {"noNetworkObjectsOrRequests", true}
    };
    if (!writeNew(manifestOutput, QJsonDocument(manifest).toJson(QJsonDocument::Indented))) return 16;
    qInfo().noquote() << "generated synthetic legacy vault" << original.size()
                     << "bytes; sha256" << sha256(original);
    return 0;
}
