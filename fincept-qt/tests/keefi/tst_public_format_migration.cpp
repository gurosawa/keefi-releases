#include "keefi/PublicFormatMigration.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif
using namespace keefi;
namespace {
QByteArray read(const QString& path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{}; }
bool write(const QString& path, const QByteArray& bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile f(path); return f.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && f.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
        && f.write(bytes) == bytes.size() && f.flush();
}
QByteArray state(const QString& format) {
    return QJsonDocument(QJsonObject{{"format", format}, {"note", "FINIX is my own label"},
                                    {"includeExtended", true}}).toJson(QJsonDocument::Compact);
}
bool fixture(const QString& path, const QString& key, const QByteArray& value) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    { QSettings s(path, QSettings::IniFormat); s.setFallbacksEnabled(false);
      s.setValue(key, value); s.setValue("unrelated/raw", QByteArray("\0FINIX\\raw", 10));
      s.sync(); if (s.status() != QSettings::NoError) return false; }
    return write(path, QByteArray("; SYNTHETIC comment must stay exactly\r\n") + read(path));
}
bool privateDirectory(const QString& path) {
    return QDir().mkpath(path)
        && QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}
QList<QByteArray> linesExcept(const QByteArray& bytes, const QByteArray& keyPrefix) {
    auto lines = bytes.split('\n');
    for (auto it = lines.begin(); it != lines.end();) {
        if (it->startsWith(keyPrefix)) it = lines.erase(it);
        else ++it;
    }
    return lines;
}
}
class PublicFormatMigrationTest final : public QObject {
    Q_OBJECT
private slots:
    void registeredFormatsKeepVersionAndUserStrings() {
        QCOMPARE(canonicalPublicFormat("finix-chart-studies/1"), QString("keefi-chart-studies/1"));
        QCOMPARE(canonicalPublicFormat("finix-chart-studies/4"), QString("keefi-chart-studies/4"));
        QCOMPARE(canonicalPublicFormat("finix-movement-colors/1"), QString("keefi-movement-colors/1"));
        QCOMPARE(canonicalPublicFormat("finix-chart-phrases/1"), QString("keefi-chart-phrases/1"));
        QCOMPARE(canonicalPublicFormat("finix-chart-studies/999"), QString("finix-chart-studies/999"));
        QCOMPARE(canonicalPublicFormat("My FINIX label"), QString("My FINIX label"));
    }
    void changesOnlyKnownValueAndKeepsOriginalCopy() {
        QTemporaryDir root; QVERIFY(root.isValid());
        const auto original=root.filePath("original.ini");
        QVERIFY(fixture(original,"chart/stock/sessions/v1",state("finix-stock-sessions/1")));
        const auto before=read(original); const auto stage=root.filePath("stage"); QVERIFY(privateDirectory(stage));
        const auto target=QDir(stage).filePath("KEEFI/KEEFI Dev.ini"); QVERIFY(write(target,before));
        QJsonArray evidence; QString error;
        QVERIFY2(convertStagedPublicSettings(stage,&evidence,&error),qPrintable(error));
        QCOMPARE(read(original),before); QCOMPARE(evidence.size(),1);
        const auto after=read(target); QVERIFY(after.startsWith("; SYNTHETIC comment must stay exactly\r\n"));
        for (const auto& line: before.split('\n')) if(!line.startsWith("stock\\sessions\\v1=")) QVERIFY(after.split('\n').contains(line));
        QSettings s(target,QSettings::IniFormat); s.setFallbacksEnabled(false);
        const auto object=QJsonDocument::fromJson(s.value("chart/stock/sessions/v1").toByteArray()).object();
        QCOMPARE(object.value("format").toString(),QString("keefi-stock-sessions/1"));
        QCOMPARE(object.value("note").toString(),QString("FINIX is my own label"));
        QVERIFY(object.value("includeExtended").toBool());
        QCOMPARE(s.value("unrelated/raw").toByteArray(),QByteArray("\0FINIX\\raw",10));
        QVERIFY2(convertStagedPublicSettings(stage,&evidence,&error),qPrintable(error));
        QVERIFY(evidence.isEmpty()); QCOMPARE(read(target),after);
    }
    void unknownSettingIsByteForByteUntouched() {
        QTemporaryDir stage; QVERIFY(stage.isValid()); const auto path=stage.filePath("KEEFI/KEEFI Dev.ini");
        QVERIFY(fixture(path,"custom/user-text",state("finix-stock-sessions/1")));
        const auto before=read(path); QJsonArray evidence; QString error;
        QVERIFY2(convertStagedPublicSettings(stage.path(),&evidence,&error),qPrintable(error));
        QVERIFY(evidence.isEmpty()); QCOMPARE(read(path),before);
    }
    void invalidKnownFormatFailsBeforeWriting_data() {
        QTest::addColumn<QByteArray>("raw");
        QTest::newRow("unknown-version") << state("finix-stock-sessions/999");
        QTest::newRow("wrong-family") << state("finix-chart-studies/1");
        QTest::newRow("malformed") << QByteArray("{invalid");
        QTest::newRow("duplicate-format") << QByteArray("{\"format\":\"finix-stock-sessions/1\",\"format\":\"finix-stock-sessions/1\",\"includeExtended\":true}");
    }
    void invalidKnownFormatFailsBeforeWriting() {
        QFETCH(QByteArray,raw); QTemporaryDir stage; QVERIFY(stage.isValid());
        const auto path=stage.filePath("KEEFI/KEEFI Dev.ini"); QVERIFY(fixture(path,"chart/stock/sessions/v1",raw));
        const auto before=read(path); QJsonArray evidence; QString error;
        QVERIFY(!convertStagedPublicSettings(stage.path(),&evidence,&error)); QVERIFY(!error.isEmpty());
        QCOMPARE(read(path),before); QVERIFY(evidence.isEmpty());
    }
    void symlinkNeverReachesOutsideStage() {
#ifdef Q_OS_UNIX
        QTemporaryDir root; QVERIFY(root.isValid()); const auto source=root.filePath("source.ini");
        QVERIFY(fixture(source,"chart/stock/sessions/v1",state("finix-stock-sessions/1")));
        const auto stage=root.filePath("stage"); QVERIFY(privateDirectory(stage)); const auto link=QDir(stage).filePath("KEEFI/KEEFI Dev.ini");
        QVERIFY(QDir().mkpath(QFileInfo(link).absolutePath())); QVERIFY(::symlink(QFile::encodeName(source).constData(),QFile::encodeName(link).constData())==0);
        const auto before=read(source); QJsonArray evidence; QString error;
        QVERIFY(!convertStagedPublicSettings(stage,&evidence,&error)); QCOMPARE(read(source),before);
#endif
    }
    void currentChartStudyPreferencesPassThroughStagingUnchanged() {
        QTemporaryDir stage; QVERIFY(stage.isValid()); QVERIFY(privateDirectory(stage.path()));
        const auto path = stage.filePath("KEEFI/KEEFI Dev.ini");
        const QByteArray raw = R"({"format":"keefi-chart-studies/5","studies":{"rsi":{"enabled":true,"visible":true},"macd":{"enabled":true,"visible":true}}})";
        QVERIFY(fixture(path, "chart/studies/v1", raw));
        const auto before = read(path);
        QJsonArray evidence; QString error;
        QVERIFY2(convertStagedPublicSettings(stage.path(), &evidence, &error), qPrintable(error));
        QCOMPARE(read(path), before);
    }
    void productionApplicationSettingsUseTheSameConversion() {
        QTemporaryDir stage; QVERIFY(stage.isValid()); QVERIFY(privateDirectory(stage.path()));
        const auto path = stage.filePath("KEEFI/KEEFI.ini");
        QVERIFY(fixture(path, "chart/stock/sessions/v1", state("finix-stock-sessions/1")));
        const auto before = read(path);
        QJsonArray evidence; QString error;
        QVERIFY2(convertStagedPublicSettings(stage.path(), &evidence, &error), qPrintable(error));
        QCOMPARE(evidence.size(), 1);
        QCOMPARE(evidence.first().toObject().value("file").toString(), QString("KEEFI/KEEFI.ini"));
        QSettings settings(path, QSettings::IniFormat); settings.setFallbacksEnabled(false);
        QCOMPARE(settings.value("chart/stock/sessions/v1").toByteArray(), state("keefi-stock-sessions/1"));
        QCOMPARE(linesExcept(read(path), "stock\\sessions\\v1="), linesExcept(before, "stock\\sessions\\v1="));
    }
    void nonPrivateStageIsRejectedWithoutChangingSettings() {
        QTemporaryDir stage; QVERIFY(stage.isValid());
        const auto path = stage.filePath("KEEFI/KEEFI Dev.ini");
        QVERIFY(fixture(path, "chart/stock/sessions/v1", state("finix-stock-sessions/1")));
        QVERIFY(QFile::setPermissions(stage.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
            | QFile::ReadGroup | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther));
        const auto before = read(path); QJsonArray evidence; QString error;
        QVERIFY(!convertStagedPublicSettings(stage.path(), &evidence, &error));
        QVERIFY(!error.isEmpty()); QVERIFY(evidence.isEmpty()); QCOMPARE(read(path), before);
    }
    void allRegisteredAliases_data() {
        QTest::addColumn<QString>("legacy");
        QTest::addColumn<QString>("current");
        const QList<QPair<QString, QString>> formats{
            {"finix-chart-studies/1", "keefi-chart-studies/1"},
            {"finix-chart-studies/2", "keefi-chart-studies/2"},
            {"finix-chart-studies/3", "keefi-chart-studies/3"},
            {"finix-chart-studies/4", "keefi-chart-studies/4"},
            {"finix-chart-drawings/1", "keefi-chart-drawings/1"},
            {"finix-chart-alert-ui/1", "keefi-chart-alert-ui/1"},
            {"finix-chart-observations/1", "keefi-chart-observations/1"},
            {"finix-watchlist-workspace/2", "keefi-watchlist-workspace/2"},
            {"finix-movement-colors/1", "keefi-movement-colors/1"},
            {"finix-stock-sessions/1", "keefi-stock-sessions/1"},
            {"finix-chart-phrases/1", "keefi-chart-phrases/1"},
            {"finix-liquidation-phrase/1", "keefi-liquidation-phrase/1"},
            {"finix-valuation-v2", "keefi-valuation-v2"}
        };
        for (const auto& pair : formats)
            QTest::newRow(qPrintable(pair.first)) << pair.first << pair.second;
    }
    void allRegisteredAliases() {
        QFETCH(QString, legacy); QFETCH(QString, current);
        QCOMPARE(canonicalPublicFormat(legacy), current);
        QCOMPARE(canonicalPublicFormat(current), current);
        QCOMPARE(canonicalPublicFormat(legacy + "/unknown"), legacy + "/unknown");
        QCOMPARE(canonicalPublicFormat("user label: " + legacy), "user label: " + legacy);
    }
    void ordinaryIniModesArePreserved_data() {
        QTest::addColumn<QString>("relative");
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("known");
        for (const auto& relative : {QString("KEEFI/KEEFI Dev.ini"), QString("KEEFI.ini")})
            for (const int mode : {0644, 0640})
                for (const bool known : {false, true}) {
                    const auto row = relative + "-" + QString::number(mode, 8)
                        + (known ? "-known" : "-unknown");
                    QTest::newRow(qPrintable(row)) << relative << mode << known;
                }
    }
    void ordinaryIniModesArePreserved() {
#ifdef Q_OS_UNIX
        QFETCH(QString, relative); QFETCH(int, mode); QFETCH(bool, known);
        QTemporaryDir root; QVERIFY(root.isValid());
        const auto stage = root.filePath("stage"); QVERIFY(privateDirectory(stage));
        const auto original = root.filePath("original.ini");
        const auto key = known ? QString("chart/stock/sessions/v1") : QString("custom/user-text");
        QVERIFY(fixture(original, key, state("finix-stock-sessions/1")));
        QCOMPARE(::chmod(QFile::encodeName(original).constData(), mode_t(mode)), 0);
        const auto before = read(original);
        const auto target = QDir(stage).filePath(relative); QVERIFY(write(target, before));
        QCOMPARE(::chmod(QFile::encodeName(target).constData(), mode_t(mode)), 0);
        struct stat sourceBefore{}, targetBefore{}, stageBefore{};
        QCOMPARE(::lstat(QFile::encodeName(original).constData(), &sourceBefore), 0);
        QCOMPARE(::lstat(QFile::encodeName(target).constData(), &targetBefore), 0);
        QCOMPARE(::lstat(QFile::encodeName(stage).constData(), &stageBefore), 0);
        QCOMPARE(int(stageBefore.st_mode & 0777), 0700);
        QCOMPARE(int(targetBefore.st_mode & 0777), mode);
        QJsonArray evidence; QString error;
        QVERIFY2(convertStagedPublicSettings(stage, &evidence, &error), qPrintable(error));
        QVERIFY(error.isEmpty());
        QCOMPARE(read(original), before);
        struct stat sourceAfter{}, targetAfter{};
        QCOMPARE(::lstat(QFile::encodeName(original).constData(), &sourceAfter), 0);
        QCOMPARE(::lstat(QFile::encodeName(target).constData(), &targetAfter), 0);
        QCOMPARE(sourceAfter.st_mode, sourceBefore.st_mode);
        QCOMPARE(sourceAfter.st_uid, sourceBefore.st_uid);
        QCOMPARE(sourceAfter.st_gid, sourceBefore.st_gid);
        QCOMPARE(targetAfter.st_mode, targetBefore.st_mode);
        QCOMPARE(targetAfter.st_uid, targetBefore.st_uid);
        QCOMPARE(targetAfter.st_gid, targetBefore.st_gid);
        QCOMPARE(evidence.size(), known ? 1 : 0);
        if (known) {
            QSettings settings(target, QSettings::IniFormat); settings.setFallbacksEnabled(false);
            const auto value = QJsonDocument::fromJson(settings.value(key).toByteArray()).object();
            QCOMPARE(value.value("format").toString(), QString("keefi-stock-sessions/1"));
            QCOMPARE(value.value("note").toString(), QString("FINIX is my own label"));
            QCOMPARE(linesExcept(read(target), "stock\\sessions\\v1="),
                     linesExcept(before, "stock\\sessions\\v1="));
        } else {
            QCOMPARE(read(target), before);
        }
#endif
    }
    void eachKnownKeyKeepsItsJsonBytes_data() {
        QTest::addColumn<QString>("relative");
        QTest::addColumn<QString>("key");
        QTest::addColumn<QString>("field");
        QTest::addColumn<QString>("legacy");
        QTest::addColumn<QString>("current");
        struct Entry { const char* key; const char* field; const char* legacy; const char* current; };
        const QList<Entry> entries{
            {"chart/studies/v1", "format", "finix-chart-studies/1", "keefi-chart-studies/1"},
            {"chart/studies/v1", "format", "finix-chart-studies/2", "keefi-chart-studies/2"},
            {"chart/studies/v1", "format", "finix-chart-studies/3", "keefi-chart-studies/3"},
            {"chart/studies/v1", "format", "finix-chart-studies/4", "keefi-chart-studies/4"},
            {"chart/drawings/v1", "schema", "finix-chart-drawings/1", "keefi-chart-drawings/1"},
            {"chart/alerts/ui/v1", "format", "finix-chart-alert-ui/1", "keefi-chart-alert-ui/1"},
            {"chart/observations/v1", "format", "finix-chart-observations/1", "keefi-chart-observations/1"},
            {"chart/watchlists/v2", "schema", "finix-watchlist-workspace/2", "keefi-watchlist-workspace/2"},
            {"display/movementColors/v1", "schema", "finix-movement-colors/1", "keefi-movement-colors/1"},
            {"chart/stock/sessions/v1", "format", "finix-stock-sessions/1", "keefi-stock-sessions/1"}
        };
        for (const auto& relative : {QString("KEEFI/KEEFI Dev.ini"), QString("KEEFI.ini")})
            for (const auto& entry : entries) {
                const auto row = relative + "-" + QLatin1String(entry.legacy);
                QTest::newRow(qPrintable(row)) << relative << QString(entry.key) << QString(entry.field)
                    << QString(entry.legacy) << QString(entry.current);
            }
    }
    void eachKnownKeyKeepsItsJsonBytes() {
        QFETCH(QString, relative); QFETCH(QString, key); QFETCH(QString, field);
        QFETCH(QString, legacy); QFETCH(QString, current);
        QTemporaryDir stage; QVERIFY(stage.isValid()); QVERIFY(privateDirectory(stage.path()));
        const auto path = stage.filePath(relative);
        const auto prefix = QByteArray("{\r\n  \"") + field.toUtf8() + "\" : \"";
        const auto suffix = QByteArray("\",\n \"nested\": {\"format\":\"") + legacy.toUtf8()
            + "\"}, \"label\":\"FINIX user label\", \"enabled\":false, \"amount\":1.2300 }\r\n";
        const auto raw = prefix + legacy.toUtf8() + suffix;
        const auto expected = prefix + current.toUtf8() + suffix;
        QVERIFY(fixture(path, key, raw));
        const auto before = read(path);
        QJsonArray evidence; QString error;
        QVERIFY2(convertStagedPublicSettings(stage.path(), &evidence, &error), qPrintable(error));
        QCOMPARE(evidence.size(), 1);
        QCOMPARE(evidence.first().toObject().value("changedKeys").toInt(), 1);
        QSettings settings(path, QSettings::IniFormat); settings.setFallbacksEnabled(false);
        QCOMPARE(settings.value(key).toByteArray(), expected);
        QCOMPARE(settings.value("unrelated/raw").toByteArray(), QByteArray("\0FINIX\\raw", 10));
        auto iniKey = key.mid(key.indexOf('/') + 1); iniKey.replace('/', '\\');
        QCOMPARE(linesExcept(read(path), iniKey.toUtf8() + '='),
                 linesExcept(before, iniKey.toUtf8() + '='));
    }
    void reviewedPackAndUnknownVariantsKeepExactBytes() {
        QTemporaryDir stage; QVERIFY(stage.isValid()); QVERIFY(privateDirectory(stage.path()));
        const auto path = stage.filePath("KEEFI/KEEFI Dev.ini");
        const QByteArray reviewed = R"PACK({ "schema" : "finix-chart-phrases/1",
 "language":"ko-KR", "note":"FINIX user label", "nested":{"format":"finix-chart-studies/4"} }
)PACK";
        const auto digest = QCryptographicHash::hash(reviewed, QCryptographicHash::Sha256).toHex();
        const auto prefix = QByteArray("{\r\n \"reviewedPack\" : \"") + reviewed.toBase64()
            + "\",\n\t\"format\"  : \"";
        const auto suffix = QByteArray("\",\n \"backgroundEnabled\":false, \"closeHintSeen\":true,"
            "\n \"dripIntroSeen\":false, \"reviewedSha256\":\"") + digest + "\"\r\n}\n";
        const auto original = prefix + "finix-chart-alert-ui/1" + suffix;
        const auto expected = prefix + "keefi-chart-alert-ui/1" + suffix;
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        const QByteArray opaque = QByteArray::fromHex("0046494e4958ff5c0001");
        const QStringList labels{"FINIX", "synthetic,value", "한글"};
        {
            QSettings settings(path, QSettings::IniFormat); settings.setFallbacksEnabled(false);
            settings.setValue("chart/alerts/ui/v1", original);
            settings.setValue("unrelated/opaque", opaque);
            settings.setValue("unrelated/labels", labels);
            settings.setValue("unrelated/count", 73);
            settings.sync(); QCOMPARE(settings.status(), QSettings::NoError);
        }
        QVERIFY(write(path, "; exact leading comment\r\n" + read(path) + "; exact trailing comment\n"));
        const auto before = read(path);
        QJsonArray evidence; QString error;
        QVERIFY2(convertStagedPublicSettings(stage.path(), &evidence, &error), qPrintable(error));
        QSettings settings(path, QSettings::IniFormat); settings.setFallbacksEnabled(false);
        const auto actual = settings.value("chart/alerts/ui/v1").toByteArray();
        QCOMPARE(actual, expected);
        const auto object = QJsonDocument::fromJson(actual).object();
        const auto retained = QByteArray::fromBase64(object.value("reviewedPack").toString().toLatin1());
        QCOMPARE(retained, reviewed);
        QCOMPARE(object.value("reviewedSha256").toString().toLatin1(), digest);
        QCOMPARE(QCryptographicHash::hash(retained, QCryptographicHash::Sha256).toHex(), digest);
        QCOMPARE(settings.value("unrelated/opaque"), QVariant(opaque));
        QCOMPARE(settings.value("unrelated/labels"), QVariant(labels));
        QCOMPARE(settings.value("unrelated/count").toInt(), 73);
        QCOMPARE(linesExcept(read(path), "alerts\\ui\\v1="),
                 linesExcept(before, "alerts\\ui\\v1="));
    }
    void invalidSecondIniLeavesBothFilesUnchanged_data() {
        QTest::addColumn<QByteArray>("raw");
        QTest::newRow("malformed") << QByteArray("{invalid");
        QTest::newRow("unknown-version") << state("finix-stock-sessions/999");
        QTest::newRow("duplicate-unrelated-decoded-key")
            << QByteArray(R"({"format":"finix-stock-sessions/1","includeExtended":true,"\u0069ncludeExtended":false})");
    }
    void invalidSecondIniLeavesBothFilesUnchanged() {
        QFETCH(QByteArray, raw);
        QTemporaryDir stage; QVERIFY(stage.isValid()); QVERIFY(privateDirectory(stage.path()));
        const auto app = stage.filePath("KEEFI/KEEFI Dev.ini");
        const auto organization = stage.filePath("KEEFI.ini");
        QVERIFY(fixture(app, "chart/stock/sessions/v1", state("finix-stock-sessions/1")));
        QVERIFY(fixture(organization, "chart/stock/sessions/v1", raw));
        const auto appBefore = read(app), organizationBefore = read(organization);
        QJsonArray evidence; QString error;
        QVERIFY(!convertStagedPublicSettings(stage.path(), &evidence, &error));
        QVERIFY(!error.isEmpty()); QVERIFY(evidence.isEmpty());
        QCOMPARE(read(app), appBefore);
        QCOMPARE(read(organization), organizationBefore);
    }
    void hardLinkedIniNeverChangesEitherName() {
#ifdef Q_OS_UNIX
        QTemporaryDir root; QVERIFY(root.isValid());
        const auto stage = root.filePath("stage"); QVERIFY(privateDirectory(stage));
        const auto original = root.filePath("original.ini");
        QVERIFY(fixture(original, "chart/stock/sessions/v1", state("finix-stock-sessions/1")));
        const auto target = QDir(stage).filePath("KEEFI.ini");
        QCOMPARE(::link(QFile::encodeName(original).constData(), QFile::encodeName(target).constData()), 0);
        struct stat linked{}; QCOMPARE(::lstat(QFile::encodeName(target).constData(), &linked), 0);
        QCOMPARE(int(linked.st_nlink), 2);
        const auto before = read(original);
        QJsonArray evidence; QString error;
        QVERIFY(!convertStagedPublicSettings(stage, &evidence, &error));
        QVERIFY(!error.isEmpty()); QVERIFY(evidence.isEmpty());
        QCOMPARE(read(original), before); QCOMPARE(read(target), before);
        struct stat after{}; QCOMPARE(::lstat(QFile::encodeName(target).constData(), &after), 0);
        QCOMPARE(after.st_ino, linked.st_ino); QCOMPARE(after.st_nlink, linked.st_nlink);
#endif
    }
    void parentSymlinkCannotRedirectSettings_data() {
        QTest::addColumn<bool>("outside");
        QTest::newRow("inside-stage") << false;
        QTest::newRow("outside-stage") << true;
    }
    void parentSymlinkCannotRedirectSettings() {
#ifdef Q_OS_UNIX
        QFETCH(bool, outside);
        QTemporaryDir root; QVERIFY(root.isValid());
        const auto stage = root.filePath("stage"); QVERIFY(privateDirectory(stage));
        const auto actualDirectory = outside ? root.filePath("outside")
            : QDir(stage).filePath("actual-settings");
        QVERIFY(privateDirectory(actualDirectory));
        const auto original = QDir(actualDirectory).filePath("KEEFI Dev.ini");
        QVERIFY(fixture(original, "chart/stock/sessions/v1", state("finix-stock-sessions/1")));
        const auto before = read(original);
        const auto link = QDir(stage).filePath("KEEFI");
        QCOMPARE(::symlink(QFile::encodeName(actualDirectory).constData(), QFile::encodeName(link).constData()), 0);
        QJsonArray evidence; QString error;
        QVERIFY(!convertStagedPublicSettings(stage, &evidence, &error));
        QVERIFY(!error.isEmpty()); QVERIFY(evidence.isEmpty());
        QCOMPARE(read(original), before);
        QVERIFY(QFileInfo(link).isSymLink());
#endif
    }
    void specialPermissionBitsAreRejected_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("setuid") << 04600;
        QTest::newRow("setgid") << 02600;
        QTest::newRow("sticky") << 01600;
    }
    void specialPermissionBitsAreRejected() {
#ifdef Q_OS_UNIX
        QFETCH(int, mode);
        QTemporaryDir stage; QVERIFY(stage.isValid()); QVERIFY(privateDirectory(stage.path()));
        const auto path = stage.filePath("KEEFI/KEEFI Dev.ini");
        QVERIFY(fixture(path, "chart/stock/sessions/v1", state("finix-stock-sessions/1")));
        QCOMPARE(::chmod(QFile::encodeName(path).constData(), mode_t(mode)), 0);
        struct stat initial{}; QCOMPARE(::lstat(QFile::encodeName(path).constData(), &initial), 0);
        QCOMPARE(int(initial.st_mode & 07777), mode);
        const auto before = read(path);
        QJsonArray evidence; QString error;
        QVERIFY(!convertStagedPublicSettings(stage.path(), &evidence, &error));
        QVERIFY(!error.isEmpty()); QVERIFY(evidence.isEmpty()); QCOMPARE(read(path), before);
        struct stat after{}; QCOMPARE(::lstat(QFile::encodeName(path).constData(), &after), 0);
        QCOMPARE(after.st_mode, initial.st_mode);
#endif
    }
};
QTEST_GUILESS_MAIN(PublicFormatMigrationTest)
#include "tst_public_format_migration.moc"
