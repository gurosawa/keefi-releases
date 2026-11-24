#include "keefi/CollectorStore.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

using namespace keefi;

namespace {
constexpr qint64 Minute = 60000, Hour = 3600000;
const qint64 H = QDateTime(QDate(2026, 11, 6), QTime(10, 0), QTimeZone::UTC).toMSecsSinceEpoch();

QByteArray line(qint64 received, const QString& id) {
    return QJsonDocument(QJsonObject{{"i", id}, {"p", "100.5"}, {"r", received}}).toJson(QJsonDocument::Compact);
}

struct Harness {
    QTemporaryDir dir;
    qint64 now = H;
    qint64 free = 200LL * 1024 * 1024 * 1024;
    CollectorStoreOptions options() const {
        CollectorStoreOptions out;
        out.root = dir.path() + QStringLiteral("/v1");
        out.collectorVersion = QStringLiteral("test");
        out.algorithmVersion = QStringLiteral("algo-1");
        return out;
    }
    std::unique_ptr<CollectorStore> make() {
        return std::make_unique<CollectorStore>(options(), [this] { return now; }, [this] { return free; });
    }
};
} // namespace

class CollectorStoreTests final : public QObject {
    Q_OBJECT
  private slots:
    void gzipRoundTripsWithinLimit() {
        bool ok = false;
        const QByteArray input(100000, 'x');
        const auto packed = collectorGzip(input, &ok);
        QVERIFY(ok);
        QVERIFY(packed.size() < input.size());
        QCOMPARE(collectorGunzip(packed, 1 << 20, &ok), input);
        QVERIFY(ok);
        QVERIFY(collectorGunzip(packed, 1000, &ok).isEmpty());
        QVERIFY(!ok);
        QVERIFY(collectorGunzip("not gzip", 1000, &ok).isEmpty());
    }

    void sealsAfterTheHourAndSettleAndVerifiesOnRead() {
        Harness harness;
        QVERIFY(harness.dir.isValid());
        auto store = harness.make();
        QVERIFY(store->open());
        harness.now = H + 10 * Minute;
        QVERIFY(store->append("trade", "binance-spot", "BTCUSDT", H + 9 * Minute, harness.now, line(harness.now, "1")));
        QVERIFY(store->append("trade", "binance-spot", "BTCUSDT", H + 59 * Minute, harness.now + 1, line(harness.now + 1, "2")));
        QVERIFY(!store->append("trade", "binance/spot", "BTCUSDT", H, harness.now, line(harness.now, "x")));
        QVERIFY(!store->append("trade", "binance-spot", "BTCUSDT", H, harness.now, "{\n}"));
        const auto base = harness.options().root + '/' + CollectorStore::relativeBase("trade", "binance-spot", "BTCUSDT", H);
        QVERIFY(QFile::exists(base + ".jsonl.part"));

        harness.now = H + Hour + 4 * Minute; // Settle not over.
        QCOMPARE(store->sealDue(), 0);
        CollectorReader reader(harness.options().root);
        QVERIFY2(reader.segments().isEmpty(), "Open segments are invisible to readers");

        harness.now = H + Hour + 5 * Minute;
        QCOMPARE(store->sealDue(), 1);
        QVERIFY(!QFile::exists(base + ".jsonl.part"));
        QVERIFY(QFile::exists(base + ".jsonl.gz"));
        QVERIFY(!(QFile::permissions(base + ".jsonl.gz") & QFileDevice::WriteOwner));
        const auto segments = reader.segments();
        QCOMPARE(segments.size(), 1);
        QCOMPARE(segments[0].records, 2);
        QCOMPARE(segments[0].origin, QStringLiteral("observed"));
        QCOMPARE(segments[0].algorithmVersion, QStringLiteral("algo-1"));
        QCOMPARE(segments[0].firstReceivedAtMs, H + 10 * Minute);
        QFile gz(base + ".jsonl.gz");
        QVERIFY(gz.open(QIODevice::ReadOnly));
        QCOMPARE(QCryptographicHash::hash(gz.readAll(), QCryptographicHash::Sha256).toHex(), segments[0].sha256.toLatin1());
        QString reason;
        const auto rows = reader.read(segments[0], &reason);
        QVERIFY2(reason.isEmpty(), qPrintable(reason));
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[1].value("i").toString(), QStringLiteral("2"));

        // Data for a sealed hour goes to a late segment; the sealed file stays untouched.
        harness.now = H + Hour + 6 * Minute;
        QVERIFY(store->append("trade", "binance-spot", "BTCUSDT", H + 30 * Minute, harness.now, line(harness.now, "late")));
        QVERIFY(QFile::exists(harness.options().root + '/' + CollectorStore::relativeBase("trade", "binance-spot", "BTCUSDT", H, 1) + ".jsonl.part"));
        QCOMPARE(store->sealDue(), 0);
        harness.now += 5 * Minute;
        QCOMPARE(store->sealDue(), 1);
        const auto all = reader.records("trade", "binance-spot", "BTCUSDT", H, H, &reason);
        QCOMPARE(all.size(), 3);
        QCOMPARE(reader.segments().last().late, 1);
    }

    void tamperedSegmentIsRejected() {
        Harness harness;
        auto store = harness.make();
        QVERIFY(store->open());
        QVERIFY(store->append("kline", "upbit", "KRW-BTC", H, H + 1, line(H + 1, "k")));
        harness.now = H + 2 * Hour;
        QCOMPARE(store->sealDue(), 1);
        CollectorReader reader(harness.options().root);
        auto segment = reader.segments().first();
        const auto path = harness.options().root + '/' + segment.path;
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QFile file(path);
        QVERIFY(file.open(QIODevice::Append));
        file.write("x");
        file.close();
        QString reason;
        QVERIFY(reader.read(segment, &reason).isEmpty());
        QVERIFY(reason.contains("해시"));
    }

    void singleInstanceAndRecoveryAfterACrash() {
        Harness harness;
        {
            auto first = harness.make();
            QVERIFY(first->open());
            auto second = harness.make();
            QString error;
            QVERIFY(!second->open(&error));
            QVERIFY(error.contains("이미"));
            QVERIFY(first->append("trade", "binance-usdm", "ZECUSDT", H + Minute, H + Minute, line(H + Minute, "a")));
            first->close(); // Keeps the .part like a killed process would.
        }
        const auto part = harness.options().root + '/' + CollectorStore::relativeBase("trade", "binance-usdm", "ZECUSDT", H) + ".jsonl.part";
        QFile torn(part);
        QVERIFY(torn.open(QIODevice::Append));
        torn.write("{\"i\":\"torn\""); // Crash in the middle of a line.
        torn.close();

        auto restarted = harness.make();
        QVERIFY(restarted->open());
        harness.now = H + 30 * Minute;
        QVERIFY(restarted->append("trade", "binance-usdm", "ZECUSDT", H + 29 * Minute, harness.now, line(harness.now, "b")));
        harness.now = H + Hour + 5 * Minute;
        QCOMPARE(restarted->sealDue(), 1);
        CollectorReader reader(harness.options().root);
        QString reason;
        const auto rows = reader.records("trade", "binance-usdm", "ZECUSDT", H, H, &reason);
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[0].value("i").toString(), QStringLiteral("a"));
        QCOMPARE(rows[1].value("i").toString(), QStringLiteral("b"));
        QCOMPARE(reader.segments().first().firstReceivedAtMs, H + Minute);
    }

    void openPartRecoveryExplainsRowsAndPartialTail_data() {
        QTest::addColumn<QByteArray>("tail");
        QTest::newRow("complete recent part") << QByteArray{};
        QTest::newRow("torn final record") << QByteArray("{\"i\":\"torn\"");
    }
    void openPartRecoveryExplainsRowsAndPartialTail() {
        QFETCH(QByteArray, tail);
        Harness harness;
        const auto relative = CollectorStore::relativeBase("trade", "binance-spot", "BTCUSDT", H) + ".jsonl.part";
        const auto path = harness.options().root + '/' + relative;
        const auto first = line(H + Minute, "first"), second = line(H + 2 * Minute, "second");
        {
            auto store = harness.make();
            QVERIFY(store->open());
            QVERIFY(store->append("trade", "binance-spot", "BTCUSDT", H + Minute, H + Minute, first));
            QVERIFY(store->append("trade", "binance-spot", "BTCUSDT", H + 2 * Minute, H + 2 * Minute, second));
            store->close(); // A recent hour stays open as .part across either stop kind.
        }
        if (!tail.isEmpty()) {
            QFile torn(path);
            QVERIFY(torn.open(QIODevice::Append));
            QCOMPARE(torn.write(tail), qint64(tail.size()));
        }
        auto restarted = harness.make();
        QVERIFY(restarted->open());
        const auto notes = restarted->recoveryNotes().join('\n');
        QVERIFY2(notes.contains(relative), qPrintable(notes));
        QVERIFY2(notes.contains(QStringLiteral("2건")), qPrintable(notes));
        QVERIFY2(notes.contains(QStringLiteral("미완성 꼬리 %1바이트").arg(tail.size())), qPrintable(notes));
        const auto restored = restarted->recentRecords("trade", H);
        QCOMPARE(restored.size(), 2);
        QCOMPARE(restored[0].object.value("i").toString(), QStringLiteral("first"));
        QCOMPARE(restored[1].object.value("i").toString(), QStringLiteral("second"));
        QCOMPARE(QFileInfo(path).size(), qint64(first.size() + second.size() + 2));
        QVERIFY(restarted->append("trade", "binance-spot", "BTCUSDT", H + 3 * Minute, H + 3 * Minute, line(H + 3 * Minute, "third")));
        harness.now = H + Hour + 5 * Minute;
        QCOMPARE(restarted->sealDue(), 1);
        CollectorReader reader(harness.options().root);
        QCOMPARE(reader.records("trade", "binance-spot", "BTCUSDT", H, H).size(), 3);
        QCOMPARE(reader.segments().first().records, 3);
    }

    void failedTornTailRecoveryBlocksAppendAndCanRetry() {
        Harness harness;
        QVERIFY(harness.dir.isValid());
        const auto relative = CollectorStore::relativeBase("trade", "binance-spot", "BTCUSDT", H) + ".jsonl.part";
        const auto path = harness.options().root + '/' + relative;
        const auto first = line(H + Minute, "first");
        const QByteArray tail("{\"i\":\"torn\"");
        {
            auto store = harness.make();
            QVERIFY(store->open());
            QVERIFY(store->append("trade", "binance-spot", "BTCUSDT", H + Minute, H + Minute, first));
            store->close();
        }
        {
            QFile torn(path);
            QVERIFY(torn.open(QIODevice::Append));
            QCOMPARE(torn.write(tail), qint64(tail.size()));
        }

        // The existing part remains writable, but its directory cannot create
        // or replace a file. Recovery must not append behind the torn JSON.
        const auto folder = QFileInfo(path).absolutePath();
        struct RestorePermissions {
            QString path;
            QFileDevice::Permissions permissions;
            ~RestorePermissions() { QFile::setPermissions(path, permissions); }
        } restore{folder, QFile::permissions(folder)};
        QVERIFY(QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        {
            QFile probe(folder + QStringLiteral("/write-probe"));
            QVERIFY2(!probe.open(QIODevice::WriteOnly), "Fixture requires temporary-file creation to be denied");
            QFile existing(path);
            QVERIFY2(existing.open(QIODevice::Append), "The existing part must still permit appending");
        }

        auto restarted = harness.make();
        QString error;
        QVERIFY2(!restarted->open(&error), "Failed torn-tail rewrite must fail open rather than accept a damaged part");
        QVERIFY(!error.isEmpty());
        QVERIFY(!restarted->isOpen());
        QVERIFY(!restarted->append("trade", "binance-spot", "BTCUSDT", H + 2 * Minute, H + 2 * Minute,
                                   line(H + 2 * Minute, "rejected")));
        {
            QFile original(path);
            QVERIFY(original.open(QIODevice::ReadOnly));
            QCOMPARE(original.readAll(), first + '\n' + tail);
        }

        QVERIFY(QFile::setPermissions(folder, restore.permissions));
        QVERIFY2(restarted->open(&error), qPrintable(error));
        QVERIFY(error.isEmpty());
        const auto restored = restarted->recentRecords("trade", H);
        QCOMPARE(restored.size(), 1);
        QCOMPARE(restored[0].object.value("i").toString(), QStringLiteral("first"));
        QVERIFY(restarted->append("trade", "binance-spot", "BTCUSDT", H + 3 * Minute, H + 3 * Minute,
                                  line(H + 3 * Minute, "after-recovery")));
        harness.now = H + Hour + 5 * Minute;
        QCOMPARE(restarted->sealDue(), 1);
        CollectorReader reader(harness.options().root);
        const auto rows = reader.records("trade", "binance-spot", "BTCUSDT", H, H, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[0].value("i").toString(), QStringLiteral("first"));
        QCOMPARE(rows[1].value("i").toString(), QStringLiteral("after-recovery"));
    }

    void diskFloorPausesRawTradesOnlyAndRecordsTheGap() {
        Harness harness;
        auto store = harness.make();
        QVERIFY(store->open());
        harness.free = 1024;
        QVERIFY(!store->append("trade", "binance-spot", "ETHUSDT", H, H, line(H, "t1")));
        QVERIFY(store->rawPaused());
        QVERIFY2(store->append("event", "binance-spot", "ETHUSDT", H, H, line(H, "e1")), "Events keep flowing");
        harness.free = 100LL * 1024 * 1024 * 1024;
        harness.now = H + 20 * Minute; // Past the cached free-space reading.
        QVERIFY(store->append("trade", "binance-spot", "ETHUSDT", H + 20 * Minute, harness.now, line(harness.now, "t2")));
        QVERIFY(!store->rawPaused());
        harness.now = H + 2 * Hour;
        QCOMPARE(store->sealDue(), 3);
        CollectorReader reader(harness.options().root);
        const auto gaps = reader.records("gap", "collector", "disk", H, H, nullptr);
        QCOMPARE(gaps.size(), 2); // Opened when the pause began, closed when it ended.
        QVERIFY(gaps[0].value("open").toBool());
        QCOMPARE(gaps[0].value("from").toInteger(), H);
        QCOMPARE(gaps[1].value("why").toString(), QStringLiteral("디스크"));
        QCOMPARE(gaps[1].value("from").toInteger(), H);
        QCOMPARE(gaps[1].value("to").toInteger(), H + 20 * Minute);
    }

    void diskPauseSurvivesStopAndCrash() {
        Harness harness;
        {
            auto store = harness.make();
            QVERIFY(store->open());
            harness.free = 1024;
            QVERIFY(!store->append("trade", "binance-spot", "ETHUSDT", H, H, line(H, "t1")));
            QVERIFY(store->writeStatus(QJsonObject{{"writtenAt", H}}));
            harness.now = H + 3 * Minute;
            store->close(); // A clean stop while paused closes the gap.
        }
        {
            harness.free = 100LL * 1024 * 1024 * 1024;
            auto store = harness.make();
            QVERIFY(store->open());
            QVERIFY(!CollectorReader(harness.options().root).status().value("rawPaused").toBool());
            harness.free = 1024;
            harness.now = H + 10 * Minute;
            QVERIFY(!store->append("trade", "binance-spot", "ETHUSDT", harness.now, harness.now, line(harness.now, "t2")));
            QVERIFY(store->writeStatus(QJsonObject{{"writtenAt", harness.now}}));
            QCOMPARE(CollectorReader(harness.options().root).status().value("rawPausedSince").toInteger(), H + 10 * Minute);
            QFile::copy(harness.options().root + "/status.json", harness.options().root + "/status.crash");
            store->close();
            // Simulate a crash: the last status still says paused and no closing record was written for it.
            QFile::remove(harness.options().root + "/status.json");
            QFile::copy(harness.options().root + "/status.crash", harness.options().root + "/status.json");
        }
        harness.free = 100LL * 1024 * 1024 * 1024;
        harness.now = H + 30 * Minute;
        auto store = harness.make();
        QVERIFY(store->open());
        harness.now = H + 2 * Hour;
        store->sealDue();
        const auto gaps = CollectorReader(harness.options().root).records("gap", "collector", "disk", H, H, nullptr);
        QStringList reasons;
        for (const auto& gap : gaps) if (gap.contains("to")) reasons.append(gap.value("why").toString());
        QVERIFY2(reasons.contains(QStringLiteral("디스크·종료")), qPrintable(reasons.join(",")));
        QVERIFY2(reasons.contains(QStringLiteral("디스크·비정상 종료")), qPrintable(reasons.join(",")));
    }

    void stopInsideASealIsFinishedOnRestartExactlyOnce_data() {
        QTest::addColumn<int>("fault");
        QTest::newRow("after compressed file") << int(CollectorSealFault::AfterCompressedFile);
        QTest::newRow("after manifest line") << int(CollectorSealFault::AfterManifestLine);
        QTest::newRow("after read-only") << int(CollectorSealFault::AfterReadOnly);
    }
    void stopInsideASealIsFinishedOnRestartExactlyOnce() {
        QFETCH(int, fault);
        Harness harness;
        {
            auto options = harness.options();
            options.testSealFault = CollectorSealFault(fault);
            CollectorStore store(options, [&] { return harness.now; }, [&] { return harness.free; });
            QVERIFY(store.open());
            QVERIFY(store.append("trade", "binance-spot", "SOLUSDT", H + Minute, H + Minute, line(H + Minute, "a")));
            QVERIFY(store.append("trade", "binance-spot", "SOLUSDT", H + 2 * Minute, H + 2 * Minute, line(H + 2 * Minute, "b")));
            harness.now = H + Hour + 5 * Minute;
            QCOMPARE(store.sealDue(), 0); // Stopped inside the seal.
        }
        auto store = harness.make();
        QVERIFY(store->open());
        const auto part = harness.options().root + '/' + CollectorStore::relativeBase("trade", "binance-spot", "SOLUSDT", H) + ".jsonl.part";
        QVERIFY2(!QFile::exists(part), "The restart finishes the seal and drops the .part");
        QVERIFY(!store->recoveryNotes().isEmpty() || fault == int(CollectorSealFault::AfterReadOnly) || fault == int(CollectorSealFault::AfterManifestLine));
        harness.now = H + 3 * Hour;
        store->sealDue();
        CollectorReader reader(harness.options().root);
        QString reason;
        const auto rows = reader.records("trade", "binance-spot", "SOLUSDT", H, H, &reason);
        QVERIFY2(reason.isEmpty(), qPrintable(reason));
        QCOMPARE(rows.size(), 2); // Exactly once.
        int entries = 0;
        for (const auto& segment : reader.segments()) entries += segment.kind == "trade" ? 1 : 0;
        QCOMPARE(entries, 1);
        const auto gz = harness.options().root + '/' + CollectorStore::relativeBase("trade", "binance-spot", "SOLUSDT", H) + ".jsonl.gz";
        QVERIFY(!(QFile::permissions(gz) & QFileDevice::WriteOwner));
    }

    void tornManifestTailDoesNotHideRecoveredSegments() {
        Harness harness;
        {
            auto store = harness.make();
            QVERIFY(store->open());
            QVERIFY(store->append("trade", "binance-spot", "BTCUSDT", H, H, line(H, "committed")));
            harness.now = H + Hour + 5 * Minute;
            QCOMPARE(store->sealDue(), 1);
        }
        {
            auto options = harness.options();
            options.testSealFault = CollectorSealFault::AfterCompressedFile;
            CollectorStore store(options, [&] { return harness.now; }, [&] { return harness.free; });
            QVERIFY(store.open());
            QVERIFY(store.append("trade", "binance-spot", "SOLUSDT", H, H, line(H, "recover")));
            QCOMPARE(store.sealDue(), 0);
        }
        QFile manifest(harness.options().root + "/manifest.jsonl");
        QVERIFY(manifest.open(QIODevice::Append));
        const QByteArray torn("{\"path\":\"trade/binance-spot/SOLUSDT/");
        QCOMPARE(manifest.write(torn), qint64(torn.size()));
        manifest.close(); // Crash during the manifest append; the .part and gzip both remain.

        auto restarted = harness.make();
        QVERIFY(restarted->open());
        CollectorReader reader(harness.options().root);
        QString reason;
        const auto segments = reader.segments(&reason);
        QVERIFY2(reason.isEmpty(), qPrintable(reason));
        QCOMPARE(segments.size(), 2);
        QCOMPARE(reader.records("trade", "binance-spot", "BTCUSDT", H, H).size(), 1);
        const auto recovered = reader.records("trade", "binance-spot", "SOLUSDT", H, H);
        QCOMPARE(recovered.size(), 1);
        QCOMPARE(recovered.first().value("i").toString(), QStringLiteral("recover"));
        restarted->close();
        restarted = harness.make();
        QVERIFY(restarted->open());
        QCOMPARE(CollectorReader(harness.options().root).segments().size(), 2);
        QCOMPARE(restarted->sealDue(), 0);
    }

    void orphanCompressedFileWithOtherContentIsSetAsideAndDataKept() {
        Harness harness;
        auto store = harness.make();
        QVERIFY(store->open());
        QVERIFY(store->append("trade", "upbit", "KRW-BTC", H + Minute, H + Minute, line(H + Minute, "x")));
        store->close();
        const auto gz = harness.options().root + '/' + CollectorStore::relativeBase("trade", "upbit", "KRW-BTC", H) + ".jsonl.gz";
        QFile orphan(gz);
        QVERIFY(orphan.open(QIODevice::WriteOnly));
        orphan.write(collectorGzip(line(H, "other") + '\n'));
        orphan.close();
        store = harness.make();
        QVERIFY(store->open());
        harness.now = H + 2 * Hour;
        QCOMPARE(store->sealDue(), 1);
        const auto rows = CollectorReader(harness.options().root).records("trade", "upbit", "KRW-BTC", H, H, nullptr);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].value("i").toString(), QStringLiteral("x"));
        QDir folder(QFileInfo(gz).absolutePath());
        QCOMPARE(folder.entryList({"*.orphan-*"}).size(), 1);
    }

    void recentRecordsReadSealedAndOpenSegments() {
        Harness harness;
        auto store = harness.make();
        QVERIFY(store->open());
        QVERIFY(store->append("trade", "binance-usdm", "BTCUSDT", H, H, line(H, "sealed")));
        harness.now = H + Hour + 6 * Minute;
        QCOMPARE(store->sealDue(), 1);
        QVERIFY(store->append("trade", "binance-usdm", "BTCUSDT", H + Hour, harness.now, line(harness.now, "open")));
        const auto records = store->recentRecords("trade", H);
        QCOMPARE(records.size(), 2);
        QVERIFY(records[0].sealed);
        QCOMPARE(records[0].venue, QStringLiteral("binance-usdm"));
        QVERIFY(!records[1].sealed);
        QCOMPARE(records[1].object.value("i").toString(), QStringLiteral("open"));
    }

    void ledgerKeepsTheFirstObservationAcrossARestart() {
        Harness harness;
        const auto trade = [](const QString& id, const QString& quantity, qint64 received) {
            return QJsonDocument(QJsonObject{{"i", id}, {"p", "100"}, {"q", quantity}, {"d", "b"}, {"T", H}, {"r", received}}).toJson(QJsonDocument::Compact);
        };
        {
            auto store = harness.make();
            QVERIFY(store->open());
            QVERIFY(store->append("trade", "binance-spot", "BTCUSDT", H, H, trade("1", "2", H)));
            store->close(); // Stop before the hour is sealed.
        }
        auto store = harness.make();
        QVERIFY(store->open());
        CollectorTradeLedger ledger;
        for (const auto& record : store->recentRecords("trade", H - Hour))
            ledger.remember(record.venue, record.symbol, record.object.value("i").toString(), CollectorTradeLedger::content(record.object),
                            record.object.value("T").toInteger());
        QCOMPARE(ledger.size(), 1);
        QByteArray first;
        QCOMPARE(ledger.observe("binance-spot", "BTCUSDT", "1", CollectorTradeLedger::content("100", "2", "", "b", H), H, &first),
                 CollectorTradeLedger::Outcome::Duplicate);
        QCOMPARE(ledger.duplicates("binance-spot", "BTCUSDT"), 1);
        QCOMPARE(ledger.observe("binance-spot", "BTCUSDT", "1", CollectorTradeLedger::content("100", "3", "", "b", H), H, &first),
                 CollectorTradeLedger::Outcome::Conflict);
        QCOMPARE(first, CollectorTradeLedger::content("100", "2", "", "b", H));
        QCOMPARE(ledger.conflicts("binance-spot", "BTCUSDT"), 1);
        QCOMPARE(ledger.observe("binance-spot", "BTCUSDT", "2", CollectorTradeLedger::content("100", "1", "", "s", H), H),
                 CollectorTradeLedger::Outcome::New);
        QCOMPARE(ledger.observe("binance-usdm", "BTCUSDT", "1", CollectorTradeLedger::content("100", "9", "", "b", H), H),
                 CollectorTradeLedger::Outcome::New); // Another venue is another ID space.
        ledger.prune(H + 1);
        QCOMPARE(ledger.size(), 0);
    }

    void reinspectionIsNotAnotherDeliveryOrConflict() {
        CollectorTradeLedger ledger;
        const auto first = CollectorTradeLedger::content("100", "2", "", "b", H);
        const auto changed = CollectorTradeLedger::content("100", "3", "", "b", H);
        QCOMPARE(ledger.observe("binance-spot", "BTCUSDT", "1", first, H, nullptr, H + 1, 1), CollectorTradeLedger::Outcome::New);
        for (int tick = 0; tick < 60; ++tick)
            QCOMPARE(ledger.observe("binance-spot", "BTCUSDT", "1", first, H, nullptr, H + 1, 1), CollectorTradeLedger::Outcome::Reinspection);
        QCOMPARE(ledger.duplicates("binance-spot", "BTCUSDT"), 0);
        QCOMPARE(ledger.observe("binance-spot", "BTCUSDT", "1", first, H, nullptr, H + 2, 1), CollectorTradeLedger::Outcome::Duplicate);
        QCOMPARE(ledger.observe("binance-spot", "BTCUSDT", "1", first, H, nullptr, H + 2, 1), CollectorTradeLedger::Outcome::Reinspection);
        QCOMPARE(ledger.duplicates("binance-spot", "BTCUSDT"), 1);
        QByteArray original;
        QCOMPARE(ledger.observe("binance-spot", "BTCUSDT", "1", changed, H, &original, H + 3, 1), CollectorTradeLedger::Outcome::Conflict);
        QCOMPARE(original, first);
        for (int tick = 0; tick < 60; ++tick)
            QCOMPARE(ledger.observe("binance-spot", "BTCUSDT", "1", changed, H, nullptr, H + 3, 1), CollectorTradeLedger::Outcome::Reinspection);
        QCOMPARE(ledger.conflicts("binance-spot", "BTCUSDT"), 1);
        // A restart preload has no current-process delivery token: the first
        // matching REST observation is a real duplicate, then only a reinspection.
        CollectorTradeLedger restarted;
        restarted.remember("binance-spot", "BTCUSDT", "1", first, H);
        QCOMPARE(restarted.observe("binance-spot", "BTCUSDT", "1", first, H, nullptr, H + Minute, 1), CollectorTradeLedger::Outcome::Duplicate);
        QCOMPARE(restarted.observe("binance-spot", "BTCUSDT", "1", first, H, nullptr, H + Minute, 1), CollectorTradeLedger::Outcome::Reinspection);
    }

    void failedRawWriteDoesNotClaimTheFirstObservation() {
        Harness harness;
        auto store = harness.make();
        QVERIFY(store->open());
        CollectorTradeLedger ledger;
        const auto content = CollectorTradeLedger::content("100", "2", "", "b", H);
        QCOMPARE(ledger.check("binance-spot", "BTCUSDT", "1", content, nullptr, H + 1, 1), CollectorTradeLedger::Outcome::New);
        harness.free = 1024;
        QVERIFY(!store->append("trade", "binance-spot", "BTCUSDT", H, H + 1, line(H + 1, "1")));
        QCOMPARE(ledger.size(), 0);
        QCOMPARE(ledger.check("binance-spot", "BTCUSDT", "1", content, nullptr, H + 1, 1), CollectorTradeLedger::Outcome::New);
        harness.free = 100LL * 1024 * 1024 * 1024;
        harness.now += Minute;
        QVERIFY(store->append("trade", "binance-spot", "BTCUSDT", H, H + 1, line(H + 1, "1")));
        QCOMPARE(ledger.observe("binance-spot", "BTCUSDT", "1", content, H, nullptr, H + 1, 1), CollectorTradeLedger::Outcome::New);
        QCOMPARE(ledger.check("binance-spot", "BTCUSDT", "1", content, nullptr, H + 1, 1), CollectorTradeLedger::Outcome::Reinspection);
        QCOMPARE(store->recentRecords("trade", H).size(), 1);
    }

    void eventRestartKeepsGroupEndAndStrongOutcomeSeparate() {
        Harness harness;
        const QJsonObject opened{{"kind", "Group"}, {"signal", "Rebound"}, {"dir", "up"}, {"from", H}, {"k", H + Minute},
                                 {"ended", false}, {"algorithm", "algo-1"}, {"r", H + Minute}};
        const QJsonObject strong{{"kind", "StrongReversal"}, {"signal", "Rebound"}, {"signalAt", H}, {"k", H + Minute},
                                 {"grade", "candidate"}, {"algorithm", "algo-1"}, {"r", H + Minute}};
        {
            auto store = harness.make();
            QVERIFY(store->open());
            for (const auto& event : {opened, strong})
                QVERIFY(store->append("event", "binance-spot", "BTCUSDT", H, H + Minute, QJsonDocument(event).toJson(QJsonDocument::Compact)));
        }
        auto store = harness.make();
        QVERIFY(store->open());
        CollectorEventLedger ledger;
        for (const auto& record : store->recentRecords("event", H)) ledger.remember(record.venue, record.symbol, record.object);
        QVERIFY(ledger.contains("binance-spot", "BTCUSDT", opened));
        QVERIFY(ledger.contains("binance-spot", "BTCUSDT", strong));
        auto ended = opened;
        ended.insert("ended", true);
        ended.insert("through", H + 5 * Minute);
        QVERIFY(!ledger.contains("binance-spot", "BTCUSDT", ended));
        ledger.remember("binance-spot", "BTCUSDT", ended);
        ended.insert("decidedAt", H + 20 * Minute); // Reinspection time is not a new event.
        QVERIFY(ledger.contains("binance-spot", "BTCUSDT", ended));
        auto promoted = strong;
        promoted.insert("grade", "strong");
        QVERIFY(!ledger.contains("binance-spot", "BTCUSDT", promoted));
        ledger.remember("binance-spot", "BTCUSDT", promoted);
        const QJsonObject outcome{{"kind", "StrongReversalOutcome"}, {"signal", "Rebound"}, {"signalAt", H},
                                  {"evaluationAt", H + Hour}, {"k", H + Hour + Minute}, {"algorithm", "algo-1"}};
        QVERIFY(!ledger.contains("binance-spot", "BTCUSDT", outcome));
        ledger.remember("binance-spot", "BTCUSDT", outcome);
        QVERIFY(ledger.contains("binance-spot", "BTCUSDT", outcome));
        QVERIFY(!ledger.contains("binance-usdm", "BTCUSDT", outcome));
        auto newAlgorithm = outcome;
        newAlgorithm.insert("algorithm", "algo-2");
        QVERIFY(!ledger.contains("binance-spot", "BTCUSDT", newAlgorithm));
    }

    void statusIsWrittenAtomically() {
        Harness harness;
        auto store = harness.make();
        QVERIFY(store->open());
        QVERIFY(store->writeStatus(QJsonObject{{"lastReceivedAt", H}}));
        CollectorReader reader(harness.options().root);
        QCOMPARE(reader.status().value("lastReceivedAt").toInteger(), H);
    }
};

QTEST_GUILESS_MAIN(CollectorStoreTests)
#include "tst_collector_store.moc"
