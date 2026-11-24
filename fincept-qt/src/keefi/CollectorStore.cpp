#include "keefi/CollectorStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStorageInfo>
#include <QTimeZone>
#ifdef Q_OS_WIN
#include <io.h>
#else
#include <unistd.h>
#endif
#include <zlib.h>

namespace keefi {
namespace {
constexpr qint64 Hour = 3600000;

bool syncFile(QFileDevice& file) {
    if (!file.flush()) return false;
    const int handle = file.handle();
#ifdef Q_OS_WIN
    return handle >= 0 && ::_commit(handle) == 0;
#else
    return handle >= 0 && ::fsync(handle) == 0;
#endif
}

QByteArray stripPartialTail(QByteArray data, qint64* records) {
    const auto last = data.lastIndexOf('\n');
    data.truncate(last < 0 ? 0 : last + 1);
    if (records) *records = data.count('\n');
    return data;
}

QByteArray sha256Hex(const QByteArray& data) { return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex(); }

struct ParsedPath {
    bool valid = false;
    QString kind, venue, symbol;
    qint64 hourStartMs = 0;
    int late = 0;
};

ParsedPath parseRelative(const QString& relative) {
    static const QRegularExpression pattern(
        "\\A([A-Za-z0-9_-]{1,32})/([A-Za-z0-9_-]{1,32})/([A-Za-z0-9_-]{1,32})/([0-9]{4})/([0-9]{2})/([0-9]{2})/([0-9]{2})(?:\\.late-([0-9]{1,4}))?\\.jsonl(?:\\.part|\\.gz)\\z");
    ParsedPath out;
    const auto match = pattern.match(relative);
    if (!match.hasMatch()) return out;
    const QDate date(match.captured(4).toInt(), match.captured(5).toInt(), match.captured(6).toInt());
    const int hour = match.captured(7).toInt();
    if (!date.isValid() || hour > 23) return out;
    out.kind = match.captured(1);
    out.venue = match.captured(2);
    out.symbol = match.captured(3);
    out.hourStartMs = QDateTime(date, QTime(hour, 0), QTimeZone::UTC).toMSecsSinceEpoch();
    out.late = match.captured(8).isEmpty() ? 0 : match.captured(8).toInt();
    out.valid = true;
    return out;
}

void receivedRange(const QByteArray& data, qint64* first, qint64* last) {
    *first = 0;
    *last = 0;
    for (const auto& line : data.split('\n')) {
        if (line.isEmpty()) continue;
        const auto received = QJsonDocument::fromJson(line).object().value("r").toInteger();
        if (received <= 0) continue;
        if (!*first || received < *first) *first = received;
        *last = qMax(*last, received);
    }
}
} // namespace

QByteArray collectorGzip(const QByteArray& input, bool* ok) {
    if (ok) *ok = false;
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return {};
    QByteArray output;
    output.resize(int(deflateBound(&stream, uLong(input.size()))) + 64);
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = uInt(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = uInt(output.size());
    const int result = deflate(&stream, Z_FINISH);
    const auto produced = stream.total_out;
    deflateEnd(&stream);
    if (result != Z_STREAM_END) return {};
    output.truncate(qsizetype(produced));
    if (ok) *ok = true;
    return output;
}

QByteArray collectorGunzip(const QByteArray& input, qsizetype maximumBytes, bool* ok) {
    if (ok) *ok = false;
    z_stream stream{};
    if (inflateInit2(&stream, 15 + 32) != Z_OK) return {};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = uInt(input.size());
    QByteArray output;
    char buffer[65536];
    int result = Z_OK;
    while (result != Z_STREAM_END) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        result = inflate(&stream, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END) { inflateEnd(&stream); return {}; }
        output.append(buffer, qsizetype(sizeof(buffer) - stream.avail_out));
        if (output.size() > maximumBytes) { inflateEnd(&stream); return {}; }
        if (result == Z_OK && stream.avail_in == 0 && stream.avail_out != 0) { inflateEnd(&stream); return {}; }
    }
    inflateEnd(&stream);
    if (ok) *ok = true;
    return output;
}

QString collectorDefaultRoot() {
    return QDir::homePath() + QStringLiteral("/Library/Application Support/KEEFI-Collector/v1");
}

struct CollectorStore::Private {
    struct Segment {
        QString kind, venue, symbol, base;
        qint64 hourStartMs = 0, records = 0, rawBytes = 0, firstReceivedAtMs = 0, lastReceivedAtMs = 0, lastAppendAtMs = 0;
        int late = 0;
        std::unique_ptr<QFile> file;
    };
    CollectorStoreOptions options;
    Clock clock;
    FreeBytes freeBytes;
    std::unique_ptr<QLockFile> lock;
    QHash<QString, std::shared_ptr<Segment>> open; // base -> segment
    QSet<QString> sealedBases;                      // Hour bases with a sealed main segment.
    QHash<QString, int> lateCounts;                 // Main hour base -> highest late index seen.
    QHash<QString, QString> manifestSha;            // Sealed relative path -> SHA-256.
    QHash<QString, CollectorKindStats> stats;
    QStringList notes;
    bool isOpen = false, paused = false;
    qint64 pausedSinceMs = 0, freeCheckedAtMs = 0, freeCached = -1;
    QString error;

    qint64 now() const { return clock ? clock() : QDateTime::currentMSecsSinceEpoch(); }
    qint64 available() {
        const auto at = now();
        if (freeCached < 0 || at - freeCheckedAtMs >= 10000) {
            freeCached = freeBytes ? freeBytes() : QStorageInfo(options.root).bytesAvailable();
            freeCheckedAtMs = at;
        }
        return freeCached;
    }
    QString absolute(const QString& relative) const { return options.root + '/' + relative; }
    QString mainBase(const Segment& segment) const { return relativeBase(segment.kind, segment.venue, segment.symbol, segment.hourStartMs); }

    int nextLate(const QString& main) {
        const int late = lateCounts.value(main) + 1;
        lateCounts.insert(main, late);
        return late;
    }

    std::shared_ptr<Segment> segmentFor(const QString& kind, const QString& venue, const QString& symbol, qint64 hourStartMs) {
        const auto main = relativeBase(kind, venue, symbol, hourStartMs);
        if (!sealedBases.contains(main)) {
            if (auto found = open.value(main)) return found;
            return create(kind, venue, symbol, hourStartMs, 0, main);
        }
        for (const auto& segment : std::as_const(open))
            if (segment->late > 0 && segment->kind == kind && segment->venue == venue && segment->symbol == symbol &&
                segment->hourStartMs == hourStartMs)
                return segment;
        const int late = nextLate(main);
        return create(kind, venue, symbol, hourStartMs, late, relativeBase(kind, venue, symbol, hourStartMs, late));
    }

    std::shared_ptr<Segment> create(const QString& kind, const QString& venue, const QString& symbol, qint64 hourStartMs, int late,
                                    const QString& base) {
        auto segment = std::make_shared<Segment>();
        segment->kind = kind;
        segment->venue = venue;
        segment->symbol = symbol;
        segment->base = base;
        segment->hourStartMs = hourStartMs;
        segment->late = late;
        const auto path = absolute(base + QStringLiteral(".jsonl.part"));
        QDir().mkpath(QFileInfo(path).absolutePath());
        segment->file = std::make_unique<QFile>(path);
        if (!segment->file->open(QIODevice::WriteOnly | QIODevice::Append)) {
            error = QStringLiteral("기록 파일을 열 수 없어요: ") + path;
            return {};
        }
        open.insert(base, segment);
        return segment;
    }

    // Steps after the compressed file exists. Each one is checked so a restart
    // can resume from wherever a previous run stopped.
    bool register_(const Segment& segment, const QString& gzRelative, const QByteArray& compressed, qint64 rawBytes, qint64 records,
                   qint64 firstReceived, qint64 lastReceived) {
        const auto sha = QString::fromLatin1(sha256Hex(compressed));
        if (!manifestSha.contains(gzRelative)) {
            QJsonObject line{{"path", gzRelative},
                             {"kind", segment.kind},
                             {"venue", segment.venue},
                             {"symbol", segment.symbol},
                             {"hour", segment.hourStartMs},
                             {"late", segment.late},
                             {"records", records},
                             {"bytes", qint64(compressed.size())},
                             {"rawBytes", rawBytes},
                             {"sha256", sha},
                             {"firstReceivedAt", firstReceived},
                             {"lastReceivedAt", lastReceived},
                             {"sealedAt", now()},
                             {"schema", options.schemaVersion},
                             {"collector", options.collectorVersion},
                             {"algorithm", options.algorithmVersion},
                             {"origin", QStringLiteral("observed")}};
            if (!repairManifestTail()) return false;
            QFile manifest(absolute(QStringLiteral("manifest.jsonl")));
            const auto bytes = QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n';
            if (!manifest.open(QIODevice::WriteOnly | QIODevice::Append) ||
                manifest.write(bytes) != bytes.size() || !syncFile(manifest)) {
                error = QStringLiteral("목록 파일에 쓸 수 없어요");
                return false;
            }
            manifestSha.insert(gzRelative, sha);
            auto& kindStats = stats[segment.kind];
            kindStats.sealedBytes += compressed.size();
            kindStats.sealedRawBytes += rawBytes;
            ++kindStats.sealedSegments;
        }
        if (options.testSealFault == CollectorSealFault::AfterManifestLine) return false;
        QFile::setPermissions(absolute(gzRelative), QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
        if (options.testSealFault == CollectorSealFault::AfterReadOnly) return false;
        QFile::remove(absolute(segment.base + QStringLiteral(".jsonl.part")));
        if (segment.late == 0) sealedBases.insert(segment.base);
        return true;
    }

    bool seal(const std::shared_ptr<Segment>& segment) {
        if (segment->file && segment->file->isOpen()) {
            if (!syncFile(*segment->file)) { error = QStringLiteral("원본 기록을 디스크에 확정할 수 없어요"); return false; }
            segment->file->close();
        }
        const auto partPath = absolute(segment->base + QStringLiteral(".jsonl.part"));
        QFile part(partPath);
        if (!part.open(QIODevice::ReadOnly)) { error = QStringLiteral("작성 중 구간을 읽을 수 없어요"); return false; }
        qint64 records = 0;
        const auto raw = stripPartialTail(part.readAll(), &records);
        part.close();
        const auto gzRelative = segment->base + QStringLiteral(".jsonl.gz");
        if (manifestSha.contains(gzRelative) || QFile::exists(absolute(gzRelative))) {
            // Only reachable if an earlier attempt in this run stopped part way.
            const auto oldBase = segment->base;
            if (resume(*segment, raw, records)) { open.remove(oldBase); return true; }
            if (segment->base != oldBase) { open.remove(oldBase); open.insert(segment->base, segment); }
            segment->file = std::make_unique<QFile>(absolute(segment->base + QStringLiteral(".jsonl.part")));
            segment->file->open(QIODevice::WriteOnly | QIODevice::Append);
            return false;
        }
        bool ok = false;
        const auto compressed = collectorGzip(raw, &ok);
        if (!ok) { error = QStringLiteral("구간 압축 실패"); return false; }
        QSaveFile out(absolute(gzRelative));
        if (!out.open(QIODevice::WriteOnly) || out.write(compressed) != compressed.size() || !syncFile(out) || !out.commit()) {
            error = QStringLiteral("확정 구간을 쓸 수 없어요");
            return false;
        }
        if (options.testSealFault == CollectorSealFault::AfterCompressedFile) return false;
        if (!register_(*segment, gzRelative, compressed, raw.size(), records, segment->firstReceivedAtMs, segment->lastReceivedAtMs)) return false;
        open.remove(segment->base);
        return true;
    }

    // A .part whose compressed twin exists: finish the seal when both hold the same
    // records, otherwise keep every record (orphan file set aside, or the .part
    // re-filed as a late segment).
    bool resume(Segment& segment, const QByteArray& raw, qint64 records) {
        const auto gzRelative = segment.base + QStringLiteral(".jsonl.gz");
        QFile gz(absolute(gzRelative));
        QByteArray compressed;
        if (gz.open(QIODevice::ReadOnly)) compressed = gz.readAll();
        bool ok = false;
        const auto unpacked = compressed.isEmpty() ? QByteArray{} : collectorGunzip(compressed, CollectorReader::MaximumSegmentBytes, &ok);
        const bool same = ok && unpacked == raw;
        if (manifestSha.contains(gzRelative)) {
            if (same && manifestSha.value(gzRelative).toLatin1() == sha256Hex(compressed)) {
                notes.append(QStringLiteral("확정 뒤 남은 작성 중 파일을 정리했어요: ") + segment.base);
                QFile::setPermissions(absolute(gzRelative), QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
                QFile::remove(absolute(segment.base + QStringLiteral(".jsonl.part")));
                if (segment.late == 0) sealedBases.insert(segment.base);
                return true;
            }
            // The sealed file holds something else: keep this data as a late segment.
            const int late = nextLate(mainBase(segment));
            const auto newBase = relativeBase(segment.kind, segment.venue, segment.symbol, segment.hourStartMs, late);
            QFile::rename(absolute(segment.base + QStringLiteral(".jsonl.part")), absolute(newBase + QStringLiteral(".jsonl.part")));
            notes.append(QStringLiteral("확정본과 다른 작성 중 파일을 늦은 구간으로 옮겼어요: ") + newBase);
            segment.base = newBase;
            segment.late = late;
            return false; // Caller keeps it open under the new base.
        }
        if (same) {
            qint64 first = 0, last = 0;
            receivedRange(raw, &first, &last);
            notes.append(QStringLiteral("압축 뒤 목록 등록 전에 멈춘 구간을 마저 등록했어요: ") + segment.base);
            return register_(segment, gzRelative, compressed, raw.size(), records, first, last);
        }
        const auto orphan = gzRelative + QStringLiteral(".orphan-%1").arg(now());
        QFile::rename(absolute(gzRelative), absolute(orphan));
        notes.append(QStringLiteral("목록에 없는 압축 파일을 따로 두고 다시 확정해요: ") + orphan);
        return false; // Caller keeps the .part open and seals it later.
    }

    bool recover() {
        QDirIterator it(options.root, {QStringLiteral("*.jsonl.part")}, QDir::Files, QDirIterator::Subdirectories);
        QList<QString> parts;
        while (it.hasNext()) parts.append(it.next());
        std::sort(parts.begin(), parts.end());
        for (const auto& path : parts) {
            const auto relative = QDir(options.root).relativeFilePath(path);
            const auto parsed = parseRelative(relative);
            if (!parsed.valid) continue;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                error = QStringLiteral("작성 중 기록을 복구하려고 읽을 수 없어요: ") + path;
                return false;
            }
            const auto originalBytes = file.size();
            qint64 records = 0;
            const auto data = stripPartialTail(file.readAll(), &records);
            if (file.error() != QFileDevice::NoError) {
                error = QStringLiteral("작성 중 기록을 끝까지 읽을 수 없어요: ") + path;
                return false;
            }
            file.close();
            auto segment = std::make_shared<Segment>();
            segment->kind = parsed.kind;
            segment->venue = parsed.venue;
            segment->symbol = parsed.symbol;
            segment->hourStartMs = parsed.hourStartMs;
            segment->late = parsed.late;
            segment->base = relative.left(relative.size() - int(QStringLiteral(".jsonl.part").size()));
            if (parsed.late > 0) {
                const auto main = relativeBase(parsed.kind, parsed.venue, parsed.symbol, parsed.hourStartMs);
                lateCounts.insert(main, qMax(lateCounts.value(main), parsed.late));
            }
            const auto gzRelative = segment->base + QStringLiteral(".jsonl.gz");
            if (manifestSha.contains(gzRelative) || QFile::exists(absolute(gzRelative)))
                if (resume(*segment, data, records)) continue;
            segment->records = records;
            segment->rawBytes = data.size();
            receivedRange(data, &segment->firstReceivedAtMs, &segment->lastReceivedAtMs);
            // Rewrite without a torn final line, then keep appending.
            const auto livePath = absolute(segment->base + QStringLiteral(".jsonl.part"));
            QSaveFile rewrite(livePath);
            if (!rewrite.open(QIODevice::WriteOnly) || rewrite.write(data) != data.size() || !syncFile(rewrite) || !rewrite.commit()) {
                error = QStringLiteral("작성 중 기록의 복구 내용을 확정할 수 없어요: ") + livePath;
                return false;
            }
            segment->file = std::make_unique<QFile>(livePath);
            if (!segment->file->open(QIODevice::WriteOnly | QIODevice::Append)) {
                error = QStringLiteral("복구한 기록을 이어 쓰려고 열 수 없어요: ") + livePath;
                return false;
            }
            segment->lastAppendAtMs = now();
            open.insert(segment->base, segment);
            notes.append(QStringLiteral("열린 기록을 복구해 이어 써요: %1 · %2건 · 미완성 꼬리 %3바이트")
                             .arg(segment->base + QStringLiteral(".jsonl.part")).arg(records).arg(originalBytes - data.size()));
        }
        return true;
    }

    bool repairManifestTail() {
        QFile manifest(absolute(QStringLiteral("manifest.jsonl")));
        if (!manifest.exists()) return true;
        if (!manifest.open(QIODevice::ReadWrite)) { error = QStringLiteral("목록 파일을 복구할 수 없어요"); return false; }
        qint64 end = manifest.size();
        if (end == 0) return true;
        if (!manifest.seek(end - 1)) return false;
        if (manifest.read(1) == "\n") return true;
        // Only a newline-terminated record is committed. Preserve all complete
        // entries; the matching .part/gzip pair re-registers an interrupted seal.
        qint64 keep = 0;
        while (end > 0) {
            const auto from = qMax<qint64>(0, end - 4096);
            if (!manifest.seek(from)) return false;
            const auto bytes = manifest.read(end - from);
            if (bytes.size() != end - from) return false;
            const auto newline = bytes.lastIndexOf('\n');
            if (newline >= 0) { keep = from + newline + 1; break; }
            end = from;
        }
        if (!manifest.resize(keep) || !syncFile(manifest)) { error = QStringLiteral("목록 파일의 미완성 줄을 복구할 수 없어요"); return false; }
        notes.append(QStringLiteral("목록 파일의 미완성 마지막 줄을 복구했어요"));
        return true;
    }

    void loadSealed() {
        QFile manifest(absolute(QStringLiteral("manifest.jsonl")));
        if (!manifest.open(QIODevice::ReadOnly)) return;
        while (!manifest.atEnd()) {
            const auto object = QJsonDocument::fromJson(manifest.readLine()).object();
            const auto path = object.value("path").toString();
            const auto parsed = parseRelative(path);
            if (!parsed.valid) continue;
            manifestSha.insert(path, object.value("sha256").toString());
            const auto main = relativeBase(parsed.kind, parsed.venue, parsed.symbol, parsed.hourStartMs);
            if (parsed.late == 0) sealedBases.insert(main);
            else lateCounts.insert(main, qMax(lateCounts.value(main), parsed.late));
        }
    }

    void gap(qint64 from, qint64 to, const QString& why, bool openGap = false) {
        QJsonObject record{{"from", from}, {"why", why}, {"r", now()}};
        if (openGap) record.insert("open", true);
        else record.insert("to", to);
        appendRaw(QStringLiteral("gap"), QStringLiteral("collector"), QStringLiteral("disk"), from, now(),
                  QJsonDocument(record).toJson(QJsonDocument::Compact));
    }

    bool appendRaw(const QString& kind, const QString& venue, const QString& symbol, qint64 bucketAtMs, qint64 receivedAtMs,
                   const QByteArray& compactJson) {
        const auto hour = bucketAtMs / Hour * Hour;
        auto segment = segmentFor(kind, venue, symbol, hour);
        if (!segment || !segment->file) return false;
        const auto line = compactJson + '\n';
        if (segment->file->write(line) != line.size()) { error = QStringLiteral("기록 쓰기 실패"); return false; }
        ++segment->records;
        segment->rawBytes += line.size();
        if (!segment->firstReceivedAtMs || receivedAtMs < segment->firstReceivedAtMs) segment->firstReceivedAtMs = receivedAtMs;
        segment->lastReceivedAtMs = qMax(segment->lastReceivedAtMs, receivedAtMs);
        segment->lastAppendAtMs = now();
        auto& kindStats = stats[kind];
        ++kindStats.records;
        kindStats.rawBytes += line.size();
        return true;
    }

    QJsonObject readStatus() const {
        QFile file(absolute(QStringLiteral("status.json")));
        if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) return {};
        return QJsonDocument::fromJson(file.readAll()).object();
    }

    bool saveStatus(QJsonObject status) {
        status.insert("rawPaused", paused);
        status.insert("rawPausedSince", paused ? pausedSinceMs : 0);
        QSaveFile file(absolute(QStringLiteral("status.json")));
        if (!file.open(QIODevice::WriteOnly)) return false;
        file.write(QJsonDocument(status).toJson(QJsonDocument::Indented));
        return syncFile(file) && file.commit();
    }
};

CollectorStore::CollectorStore(CollectorStoreOptions options, Clock clock, FreeBytes freeBytes) : d_(std::make_unique<Private>()) {
    d_->options = std::move(options);
    d_->clock = std::move(clock);
    d_->freeBytes = std::move(freeBytes);
}

CollectorStore::~CollectorStore() { close(); }

bool CollectorStore::validName(const QString& name) {
    static const QRegularExpression pattern("\\A[A-Za-z0-9_-]{1,32}\\z");
    return pattern.match(name).hasMatch();
}

QString CollectorStore::relativeBase(const QString& kind, const QString& venue, const QString& symbol, qint64 hourStartMs, int late) {
    const auto at = QDateTime::fromMSecsSinceEpoch(hourStartMs, QTimeZone::UTC);
    auto base = QStringLiteral("%1/%2/%3/%4/%5")
                    .arg(kind, venue, symbol, at.toString(QStringLiteral("yyyy/MM/dd")), at.toString(QStringLiteral("HH")));
    if (late > 0) base += QStringLiteral(".late-%1").arg(late);
    return base;
}

bool CollectorStore::open(QString* error) {
    const auto fail = [&](const QString& why) { d_->error = why; if (error) *error = why; return false; };
    if (d_->isOpen) return true;
    d_->error.clear();
    if (d_->options.root.isEmpty() || !QDir().mkpath(d_->options.root)) return fail(QStringLiteral("기록 폴더를 만들 수 없어요"));
    d_->lock = std::make_unique<QLockFile>(d_->options.root + QStringLiteral("/.lock"));
    if (!d_->lock->tryLock(0)) { d_->lock.reset(); return fail(QStringLiteral("다른 수집기가 이미 이 폴더를 쓰고 있어요")); }
    if (!d_->repairManifestTail()) { d_->lock.reset(); return fail(d_->error); }
    d_->loadSealed();
    if (!d_->recover()) {
        // Earlier segments may already be open. Close them and release the
        // writer lock so a corrected filesystem can be retried safely.
        d_->open.clear();
        d_->lock.reset();
        return fail(d_->error);
    }
    d_->isOpen = true;
    // A run that stopped while raw trades were paused never wrote the end of that gap.
    auto status = d_->readStatus();
    const auto since = status.value("rawPausedSince").toInteger();
    if (status.value("rawPaused").toBool() && since > 0) {
        d_->gap(since, d_->now(), QStringLiteral("디스크·비정상 종료"));
        d_->saveStatus(status);
    }
    if (error) error->clear();
    return true;
}

bool CollectorStore::isOpen() const { return d_->isOpen; }

void CollectorStore::close() {
    if (!d_->isOpen) return;
    if (d_->paused) {
        d_->gap(d_->pausedSinceMs, d_->now(), QStringLiteral("디스크·종료"));
        d_->paused = false;
        auto status = d_->readStatus();
        if (!status.isEmpty()) d_->saveStatus(status);
    }
    for (const auto& segment : std::as_const(d_->open))
        if (segment->file && segment->file->isOpen()) { syncFile(*segment->file); segment->file->close(); }
    d_->open.clear();
    d_->isOpen = false;
    if (d_->lock) d_->lock->unlock();
    d_->lock.reset();
}

bool CollectorStore::append(const QString& kind, const QString& venue, const QString& symbol, qint64 bucketAtMs, qint64 receivedAtMs,
                            const QByteArray& compactJson) {
    if (!d_->isOpen || !validName(kind) || !validName(venue) || !validName(symbol) || bucketAtMs <= 0 || receivedAtMs <= 0 ||
        compactJson.isEmpty() || compactJson.contains('\n'))
        return false;
    const auto at = d_->now();
    if (kind == QLatin1String("trade")) {
        const bool low = d_->available() < d_->options.diskFloorBytes;
        if (low && !d_->paused) {
            d_->paused = true;
            d_->pausedSinceMs = at;
            d_->gap(at, 0, QStringLiteral("디스크"), true); // Written now so a stop cannot lose the start.
        }
        if (!low && d_->paused) {
            d_->paused = false;
            d_->gap(d_->pausedSinceMs, at, QStringLiteral("디스크"));
        }
        if (d_->paused) return false;
    }
    return d_->appendRaw(kind, venue, symbol, bucketAtMs, receivedAtMs, compactJson);
}

int CollectorStore::sealDue() {
    if (!d_->isOpen) return 0;
    const auto at = d_->now();
    QList<std::shared_ptr<Private::Segment>> due;
    for (const auto& segment : std::as_const(d_->open)) {
        const bool hourDone = at >= segment->hourStartMs + Hour + d_->options.settleMs;
        const bool lateQuiet = segment->late == 0 || at >= segment->lastAppendAtMs + d_->options.settleMs;
        if (hourDone && lateQuiet) due.append(segment);
    }
    // Main segments first so a late segment never precedes its hour in the manifest.
    std::sort(due.begin(), due.end(), [](const auto& a, const auto& b) { return a->late != b->late ? a->late < b->late : a->base < b->base; });
    int sealed = 0;
    for (const auto& segment : due)
        if (d_->seal(segment)) ++sealed;
    return sealed;
}

void CollectorStore::flush() {
    for (const auto& segment : std::as_const(d_->open))
        if (segment->file && segment->file->isOpen()) segment->file->flush();
}

bool CollectorStore::rawPaused() const { return d_->paused; }

bool CollectorStore::writeStatus(const QJsonObject& status) { return d_->isOpen && d_->saveStatus(status); }

QList<CollectorStoredRecord> CollectorStore::recentRecords(const QString& kind, qint64 sinceHourMs) {
    QList<CollectorStoredRecord> out;
    CollectorReader reader(d_->options.root);
    for (const auto& segment : reader.segments()) {
        if (segment.kind != kind || segment.hourStartMs < sinceHourMs) continue;
        for (const auto& object : reader.read(segment))
            out.append({segment.kind, segment.venue, segment.symbol, segment.hourStartMs, true, object});
    }
    flush();
    for (const auto& segment : std::as_const(d_->open)) {
        if (segment->kind != kind || segment->hourStartMs < sinceHourMs) continue;
        QFile file(d_->absolute(segment->base + QStringLiteral(".jsonl.part")));
        if (!file.open(QIODevice::ReadOnly)) continue;
        for (const auto& line : stripPartialTail(file.readAll(), nullptr).split('\n')) {
            if (line.isEmpty()) continue;
            const auto doc = QJsonDocument::fromJson(line);
            if (doc.isObject()) out.append({segment->kind, segment->venue, segment->symbol, segment->hourStartMs, false, doc.object()});
        }
    }
    return out;
}

QStringList CollectorStore::recoveryNotes() const { return d_->notes; }
QHash<QString, CollectorKindStats> CollectorStore::stats() const { return d_->stats; }
QString CollectorStore::root() const { return d_->options.root; }
QString CollectorStore::lastError() const { return d_->error; }

QByteArray CollectorTradeLedger::content(const QString& price, const QString& quantity, const QString& normalQuantity, const QString& side,
                                         qint64 tradeAtMs) {
    return (price + '|' + quantity + '|' + normalQuantity + '|' + side + '|' + QString::number(tradeAtMs)).toUtf8();
}

QByteArray CollectorTradeLedger::content(const QJsonObject& trade) {
    return content(trade.value("p").toString(), trade.value("q").toString(), trade.value("n").toString(), trade.value("d").toString(),
                   trade.value("T").toInteger());
}

void CollectorTradeLedger::remember(const QString& venue, const QString& symbol, const QString& id, const QByteArray& content, qint64 tradeAtMs) {
    const auto key = venue + '|' + symbol + '|' + id;
    if (!entries_.contains(key)) entries_.insert(key, {content, tradeAtMs}); // The first stored observation wins.
}

CollectorTradeLedger::Outcome CollectorTradeLedger::check(const QString& venue, const QString& symbol, const QString& id,
                                                         const QByteArray& content, QByteArray* firstContent, qint64 receivedAtMs,
                                                         quint64 epoch) const {
    const auto key = venue + '|' + symbol + '|' + id;
    const auto found = entries_.constFind(key);
    if (found == entries_.cend()) return Outcome::New;
    if (firstContent) *firstContent = found->content;
    if (receivedAtMs > 0 && found->lastReceivedAtMs == receivedAtMs && found->lastEpoch == epoch && found->lastContent == content)
        return Outcome::Reinspection;
    return found->content == content ? Outcome::Duplicate : Outcome::Conflict;
}

CollectorTradeLedger::Outcome CollectorTradeLedger::observe(const QString& venue, const QString& symbol, const QString& id,
                                                            const QByteArray& content, qint64 tradeAtMs, QByteArray* firstContent,
                                                            qint64 receivedAtMs, quint64 epoch, qint64 inspectedAtMs) {
    const auto outcome = check(venue, symbol, id, content, firstContent, receivedAtMs, epoch);
    if (outcome == Outcome::New) remember(venue, symbol, id, content, tradeAtMs);
    auto& entry = entries_[venue + '|' + symbol + '|' + id];
    entry.lastContent = content;
    entry.lastReceivedAtMs = receivedAtMs;
    entry.lastEpoch = epoch;
    entry.lastInspectedAtMs = qMax(entry.lastInspectedAtMs, inspectedAtMs);
    if (outcome == Outcome::Duplicate) ++duplicates_[venue + '|' + symbol];
    if (outcome == Outcome::Conflict) ++conflicts_[venue + '|' + symbol];
    return outcome;
}

void CollectorTradeLedger::prune(qint64 olderThanTradeAtMs) {
    for (auto it = entries_.begin(); it != entries_.end();)
        it = qMax(it->tradeAtMs, it->lastInspectedAtMs) < olderThanTradeAtMs ? entries_.erase(it) : std::next(it);
}

qint64 CollectorTradeLedger::duplicates(const QString& venue, const QString& symbol) const { return duplicates_.value(venue + '|' + symbol); }
qint64 CollectorTradeLedger::conflicts(const QString& venue, const QString& symbol) const { return conflicts_.value(venue + '|' + symbol); }

QByteArray CollectorEventLedger::key(const QString& venue, const QString& symbol, const QJsonObject& event) {
    QJsonObject identity{{"venue", venue}, {"symbol", symbol}, {"algorithm", event.value("algorithm")}, {"kind", event.value("kind")}};
    const auto kind = event.value("kind").toString();
    QStringList fields;
    if (kind == QLatin1String("Signal")) fields = {"signal", "dir", "at", "k"};
    else if (kind == QLatin1String("Group")) fields = {"signal", "dir", "from", "k", "ended"};
    else if (kind == QLatin1String("LargeTradePoint")) fields = {"i", "T"};
    else if (kind == QLatin1String("StrongReversal")) fields = {"signal", "signalAt", "k", "grade"};
    else if (kind == QLatin1String("StrongReversalOutcome")) fields = {"signal", "signalAt", "evaluationAt", "k"};
    else return {};
    for (const auto& field : fields) identity.insert(field, event.value(field));
    return QJsonDocument(identity).toJson(QJsonDocument::Compact);
}

bool CollectorEventLedger::contains(const QString& venue, const QString& symbol, const QJsonObject& event) const {
    return keys_.contains(key(venue, symbol, event));
}

void CollectorEventLedger::remember(const QString& venue, const QString& symbol, const QJsonObject& event) {
    const auto identity = key(venue, symbol, event);
    if (!identity.isEmpty()) keys_.insert(identity);
}

CollectorReader::CollectorReader(QString root) : root_(std::move(root)) {}

QList<CollectorSegment> CollectorReader::segments(QString* reason) const {
    QList<CollectorSegment> out;
    QFile manifest(root_ + QStringLiteral("/manifest.jsonl"));
    if (!manifest.open(QIODevice::ReadOnly)) {
        if (reason) *reason = QStringLiteral("수집기 기록이 아직 없어요");
        return out;
    }
    int malformed = 0;
    QSet<QString> seen;
    while (!manifest.atEnd()) {
        const auto line = manifest.readLine().trimmed();
        if (line.isEmpty()) continue;
        const auto object = QJsonDocument::fromJson(line).object();
        CollectorSegment segment;
        segment.path = object.value("path").toString();
        const auto parsed = parseRelative(segment.path);
        segment.sha256 = object.value("sha256").toString();
        segment.schema = object.value("schema").toInt();
        if (!parsed.valid || !segment.path.endsWith(QStringLiteral(".jsonl.gz")) || segment.sha256.size() != 64 || segment.schema != 1 ||
            object.value("kind").toString() != parsed.kind || object.value("venue").toString() != parsed.venue ||
            object.value("symbol").toString() != parsed.symbol || object.value("hour").toInteger() != parsed.hourStartMs ||
            object.value("late").toInt() != parsed.late) {
            ++malformed;
            continue;
        }
        if (seen.contains(segment.path)) continue; // One entry per sealed file.
        seen.insert(segment.path);
        segment.kind = parsed.kind;
        segment.venue = parsed.venue;
        segment.symbol = parsed.symbol;
        segment.hourStartMs = parsed.hourStartMs;
        segment.late = parsed.late;
        segment.records = object.value("records").toInteger();
        segment.bytes = object.value("bytes").toInteger();
        segment.firstReceivedAtMs = object.value("firstReceivedAt").toInteger();
        segment.lastReceivedAtMs = object.value("lastReceivedAt").toInteger();
        segment.sealedAtMs = object.value("sealedAt").toInteger();
        segment.origin = object.value("origin").toString();
        segment.collectorVersion = object.value("collector").toString();
        segment.algorithmVersion = object.value("algorithm").toString();
        out.append(segment);
    }
    if (reason) *reason = malformed ? QStringLiteral("목록의 %1줄은 형식이 맞지 않아 읽지 않았어요").arg(malformed) : QString{};
    return out;
}

QList<QJsonObject> CollectorReader::read(const CollectorSegment& segment, QString* reason) const {
    const auto fail = [&](const QString& why) { if (reason) *reason = why; return QList<QJsonObject>{}; };
    QFile file(root_ + '/' + segment.path);
    if (!file.open(QIODevice::ReadOnly)) return fail(QStringLiteral("확정 구간 파일이 없어요"));
    const auto compressed = file.readAll();
    if (sha256Hex(compressed) != segment.sha256.toLatin1()) return fail(QStringLiteral("확정 구간의 해시가 목록과 달라요"));
    bool ok = false;
    const auto raw = collectorGunzip(compressed, MaximumSegmentBytes, &ok);
    if (!ok) return fail(QStringLiteral("확정 구간을 풀 수 없어요"));
    QList<QJsonObject> out;
    for (const auto& line : raw.split('\n')) {
        if (line.isEmpty()) continue;
        const auto doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) return fail(QStringLiteral("확정 구간의 기록 형식이 맞지 않아요"));
        out.append(doc.object());
    }
    if (reason) reason->clear();
    return out;
}

QList<QJsonObject> CollectorReader::records(const QString& kind, const QString& venue, const QString& symbol, qint64 fromHourMs,
                                            qint64 toHourMs, QString* reason) const {
    QList<QJsonObject> out;
    QString why;
    for (const auto& segment : segments(&why)) {
        if (segment.kind != kind || segment.venue != venue || segment.symbol != symbol || segment.hourStartMs < fromHourMs ||
            segment.hourStartMs > toHourMs)
            continue;
        QString readWhy;
        const auto rows = read(segment, &readWhy);
        if (!readWhy.isEmpty()) { why = readWhy; continue; }
        out += rows;
    }
    if (reason) *reason = why;
    return out;
}

QJsonObject CollectorReader::status(QString* reason) const {
    QFile file(root_ + QStringLiteral("/status.json"));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) {
        if (reason) *reason = QStringLiteral("수집기 상태 파일이 없어요");
        return {};
    }
    const auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        if (reason) *reason = QStringLiteral("수집기 상태 파일 형식이 맞지 않아요");
        return {};
    }
    if (reason) reason->clear();
    return doc.object();
}
} // namespace keefi
