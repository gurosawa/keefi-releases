#include "keefi/PlatformSecurity.h"
#include "keefi/SecretWorker.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <cstdlib>
#include <memory>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#ifndef KEEFI_SECURITY_FIXTURE
#error KEEFI_SECURITY_FIXTURE must identify the absolute source fixture path
#endif
#ifndef KEEFI_TEST_PYTHON
#error KEEFI_TEST_PYTHON must identify the absolute test interpreter path
#endif

using keefi::PlatformSecurity;
using keefi::SecretWorker;

#ifdef Q_OS_MACOS
void keefiPostNativeSessionInactiveForTest();
void keefiPostNativeSleepForTest();
#endif

class SecurityTest final : public QObject {
    Q_OBJECT
private:
    PlatformSecurity security_;
    QString secret_;
    QTemporaryDir profile_;
    QString fixture_;
    QByteArray hash_;

    QByteArray payload(const char* mode = "normal", int otherFd = -1) const {
        QJsonObject object{{QStringLiteral("secret"), secret_},
                           {QStringLiteral("mode"), QString::fromLatin1(mode)}};
        if (otherFd >= 0) object.insert(QStringLiteral("otherFd"), otherFd);
        return QJsonDocument(object).toJson(QJsonDocument::Compact);
    }
    std::unique_ptr<SecretWorker> worker() {
        return std::make_unique<SecretWorker>(&security_, QString::fromUtf8(KEEFI_TEST_PYTHON), fixture_, hash_);
    }
    bool noPlaintextOrDumpInProfile() {
        QDirIterator files(profile_.path(), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString path = files.next();
            if (QFileInfo(path).fileName().startsWith(QStringLiteral("core")) || path.endsWith(QStringLiteral(".dmp")))
                return false;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || file.readAll().contains(secret_.toUtf8())) return false;
        }
        return true;
    }

private slots:
    void initTestCase() {
        QVERIFY(profile_.isValid());
        secret_ = QStringLiteral("KEEFI_FAKE_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        fixture_ = profile_.filePath(QStringLiteral("fake_worker.py"));
        QVERIFY(QFile::copy(QString::fromUtf8(KEEFI_SECURITY_FIXTURE), fixture_));
        QFile script(fixture_);
        QVERIFY(script.open(QIODevice::ReadOnly));
        hash_ = QCryptographicHash::hash(script.readAll(), QCryptographicHash::Sha256);
#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
        QVERIFY(!security_.initialize());
        QCOMPARE(security_.statusCode(), QStringLiteral("platform_secret_protection_unsupported"));
        QSKIP("KEEFI G1 secret operations are unsupported on this OS");
#else
        QVERIFY2(security_.initialize(), qPrintable(security_.statusCode()));
#endif
    }

    void hardLimitAndEnvironment() {
        QString error;
        QVERIFY(PlatformSecurity::hardenCurrentProcess(&error));
#ifdef Q_OS_MACOS
        rlimit limit{};
        QCOMPARE(getrlimit(RLIMIT_CORE, &limit), 0);
        QCOMPARE(limit.rlim_cur, rlim_t(0));
        QCOMPARE(limit.rlim_max, rlim_t(0));
#endif
        qputenv("KEEFI_PARENT_SECRET", secret_.toUtf8());
        const auto environment = SecretWorker::isolatedEnvironment();
        QVERIFY(!environment.contains(QStringLiteral("KEEFI_PARENT_SECRET")));
        QVERIFY(!environment.contains(QStringLiteral("PYTHONPATH")));
        QVERIFY(!environment.contains(QStringLiteral("HOME")));
        qunsetenv("KEEFI_PARENT_SECRET");
    }

    void isolatedPipeAndProcessList() {
        qputenv("KEEFI_PARENT_SECRET", secret_.toUtf8());
        auto task = worker();
        QSignalSpy result(task.get(), &SecretWorker::finished);
#ifdef Q_OS_UNIX
        const int fd = open("/dev/null", O_RDONLY);
        QVERIFY(fd >= 0);
        // Deliberately inheritable: QProcess must close this unrelated fd.
        QVERIFY(fcntl(fd, F_SETFD, 0) == 0);
        QVERIFY(task->start(payload("hold", fd)));
#else
        QVERIFY(task->start(payload("hold")));
#endif
        QTRY_VERIFY_WITH_TIMEOUT(task->processId() > 0, 2000);
#ifdef Q_OS_MACOS
        QProcess ps;
        ps.setProcessEnvironment(SecretWorker::isolatedEnvironment());
        ps.start(QStringLiteral("/bin/ps"), {QStringLiteral("eww"), QStringLiteral("-p"),
                                           QString::number(task->processId()), QStringLiteral("-o"), QStringLiteral("command=")});
        QVERIFY(ps.waitForFinished(2000));
        const QByteArray listing = ps.readAllStandardOutput();
        QVERIFY(!listing.isEmpty());
        QVERIFY(!listing.contains(secret_.toUtf8()));
        QVERIFY(!listing.contains("KEEFI_PARENT_SECRET"));
#endif
        QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 5000);
        QVERIFY(result.at(0).at(1).toBool());
        QCOMPARE(result.at(0).at(2).toString(), QStringLiteral("ok"));
#ifdef Q_OS_UNIX
        close(fd);
#endif
        qunsetenv("KEEFI_PARENT_SECRET");
        QVERIFY(noPlaintextOrDumpInProfile());
    }

    void lockStopsAllAndStaleResults() {
        auto first = worker();
        auto second = worker();
        QSignalSpy a(first.get(), &SecretWorker::finished);
        QSignalSpy b(second.get(), &SecretWorker::finished);
        QVERIFY(first->start(payload("hold")));
        QVERIFY(second->start(payload("hold")));
        QTRY_VERIFY(first->processId() > 0 && second->processId() > 0);
        security_.requestLock(QStringLiteral("test_lock_event"));
        QCOMPARE(a.count(), 1);
        QCOMPARE(b.count(), 1);
        QVERIFY(!first->running());
        QVERIFY(!second->running());
        QCOMPARE(a.at(0).at(2).toString(), QStringLiteral("locked_or_cancelled"));
        QVERIFY(first->start(payload()));
        QTRY_COMPARE_WITH_TIMEOUT(a.count(), 2, 3000);
        QVERIFY(a.at(1).at(1).toBool());
        QVERIFY(a.at(0).at(0) != a.at(1).at(0));
        QTest::qWait(100);
        QCOMPARE(a.count(), 2);
        QCOMPARE(b.count(), 1);
    }

    void nativeObserverNotificationPath() {
#ifdef Q_OS_MACOS
        QSignalSpy locks(&security_, &PlatformSecurity::lockRequested);
        auto task = worker();
        QSignalSpy result(task.get(), &SecretWorker::finished);
        QVERIFY(task->start(payload("hold")));
        QTRY_VERIFY(task->processId() > 0);
        keefiPostNativeSessionInactiveForTest();
        QCOMPARE(locks.count(), 1);
        QCOMPARE(locks.at(0).at(0).toString(), QStringLiteral("session_inactive"));
        QCOMPARE(result.count(), 1);
        QVERIFY(!task->running());
        keefiPostNativeSleepForTest();
        QCOMPARE(locks.count(), 2);
        QCOMPARE(locks.at(1).at(0).toString(), QStringLiteral("system_sleep_or_resume"));
#else
        QSKIP("Native notification injection is currently a macOS-only fixture");
#endif
    }

    void keepAwakeReleasesOnLock() {
        QVERIFY(!security_.keepAwake());
        QVERIFY(security_.setKeepAwake(true));
        QVERIFY(security_.keepAwake());
        security_.requestLock(QStringLiteral("test_lock_event"));
        QVERIFY(!security_.keepAwake());
    }

    void timeoutDoesNotRetry() {
        auto task = worker();
        QSignalSpy result(task.get(), &SecretWorker::finished);
        QVERIFY(task->start(payload("timeout"), 250));
        QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 2000);
        QCOMPARE(result.at(0).at(2).toString(), QStringLiteral("timeout"));
        QVERIFY(!task->running());
        QTest::qWait(100);
        QCOMPARE(result.count(), 1);
    }

    void protocolAndOutput_data() {
        QTest::addColumn<QByteArray>("mode");
        QTest::addColumn<bool>("success");
        QTest::addColumn<QString>("code");
        QTest::newRow("stderr-secret-is-discarded") << QByteArray("stderr") << true << QStringLiteral("ok");
        QTest::newRow("wrong-request-id") << QByteArray("bad-id") << false << QStringLiteral("protocol_error");
        QTest::newRow("echo-is-rejected") << QByteArray("echo") << false << QStringLiteral("protocol_error");
        QTest::newRow("oversize-is-rejected") << QByteArray("oversize") << false << QStringLiteral("protocol_error");
    }
    void protocolAndOutput() {
        QFETCH(QByteArray, mode);
        QFETCH(bool, success);
        QFETCH(QString, code);
        auto task = worker();
        QSignalSpy result(task.get(), &SecretWorker::finished);
        QVERIFY(task->start(payload(mode.constData())));
        QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 3000);
        QCOMPARE(result.at(0).at(1).toBool(), success);
        QCOMPARE(result.at(0).at(2).toString(), code);
        for (const QVariant& value : result.at(0)) QVERIFY(!value.toString().contains(secret_));
        QVERIFY(noPlaintextOrDumpInProfile());
    }

    void invalidConfiguration() {
        SecretWorker wrongHash(&security_, QString::fromUtf8(KEEFI_TEST_PYTHON), fixture_, QByteArray(32, 'x'));
        QVERIFY(!wrongHash.start(payload()));
        SecretWorker relative(&security_, QStringLiteral("python3"), fixture_, hash_);
        QVERIFY(!relative.start(payload()));
        auto task = worker();
        QVERIFY(!task->start(QByteArray(64 * 1024 + 1, 'x')));
        QVERIFY(!task->start(payload(), 0));
    }

    void fakeSecretWorkerCrash() {
        if (qEnvironmentVariableIntValue("KEEFI_RUN_CRASH_PROBES") != 1)
            QSKIP("Intentional Python crash is opt-in; enable KEEFI_ENABLE_CRASH_PROBES only for a dedicated dump audit.");
        auto task = worker();
        QSignalSpy result(task.get(), &SecretWorker::finished);
        QVERIFY(task->start(payload("crash")));
        QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 5000);
        QVERIFY(!result.at(0).at(1).toBool());
        QVERIFY(!task->running());
        QVERIFY(noPlaintextOrDumpInProfile());
    }

    void fakeSecretNativeCrash() {
        if (qEnvironmentVariableIntValue("KEEFI_RUN_CRASH_PROBES") != 1)
            QSKIP("Intentional native crash is opt-in; enable KEEFI_ENABLE_CRASH_PROBES only for a dedicated dump audit.");
        QProcess probe;
        probe.setProcessEnvironment(SecretWorker::isolatedEnvironment());
        probe.setWorkingDirectory(profile_.path());
        probe.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--keefi-crash-probe")});
        QVERIFY(probe.waitForStarted(2000));
        probe.write(secret_.toUtf8());
        probe.closeWriteChannel();
        QVERIFY(probe.waitForFinished(5000));
        QCOMPARE(probe.exitStatus(), QProcess::CrashExit);
        QVERIFY(!probe.readAllStandardOutput().contains(secret_.toUtf8()));
        QVERIFY(!probe.readAllStandardError().contains(secret_.toUtf8()));
        QVERIFY(noPlaintextOrDumpInProfile());
    }

    void cleanupTestCase() { qunsetenv("KEEFI_PARENT_SECRET"); }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--keefi-crash-probe"))) {
        QString error;
        if (!PlatformSecurity::hardenCurrentProcess(&error)) return 2;
        QFile input;
        if (!input.open(stdin, QIODevice::ReadOnly)) return 3;
        const QByteArray fakeSecret = input.readAll();
        if (!fakeSecret.startsWith("KEEFI_FAKE_")) return 4;
        std::abort();
    }
    SecurityTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_security.moc"
