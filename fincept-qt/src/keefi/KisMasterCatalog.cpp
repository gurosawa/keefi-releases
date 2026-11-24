#include "keefi/KisMasterCatalog.h"
#include "keefi/ProductIdentity.h"
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>
#include <limits>
#include <zlib.h>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <iconv.h>
#endif

namespace keefi {
namespace {
constexpr quint32 MaxCompressed = 4 * 1024 * 1024, MaxExpanded = 16 * 1024 * 1024;
quint16 u16(const QByteArray& b, qsizetype p) { return qFromLittleEndian<quint16>(b.constData() + p); }
quint32 u32(const QByteArray& b, qsizetype p) { return qFromLittleEndian<quint32>(b.constData() + p); }
bool span(const QByteArray& b, quint64 p, quint64 n) { return p <= quint64(b.size()) && n <= quint64(b.size()) - p; }
bool decodeCp949(const QByteArray& raw, QString& text) {
#ifdef Q_OS_WIN
    const int length = MultiByteToWideChar(949, MB_ERR_INVALID_CHARS, raw.constData(), int(raw.size()), nullptr, 0);
    if (length <= 0) return false;
    std::wstring wide(size_t(length), L'\0');
    if (MultiByteToWideChar(949, MB_ERR_INVALID_CHARS, raw.constData(), int(raw.size()), wide.data(), length) != length) return false;
    text = QString::fromWCharArray(wide.data(), length);
#else
    iconv_t converter = iconv_open("UTF-8", "CP949");
    if (converter == reinterpret_cast<iconv_t>(-1)) return false;
    QByteArray utf8(raw.size() * 3 + 4, '\0');
    char* input = const_cast<char*>(raw.constData()); char* output = utf8.data();
    size_t inLeft = size_t(raw.size()), outLeft = size_t(utf8.size());
    const size_t result = iconv(converter, &input, &inLeft, &output, &outLeft);
    iconv_close(converter);
    if (result == size_t(-1) || inLeft != 0) return false;
    utf8.resize(utf8.size() - qsizetype(outLeft));
    text = QString::fromUtf8(utf8);
#endif
    return !text.contains(QChar::ReplacementCharacter);
}
bool unzip(const QByteArray& bytes, const QByteArray& expected, QByteArray& expanded) {
    if (bytes.size() < 98 || bytes.size() > MaxCompressed) return false;
    // A bounded single-entry archive, no split archive, ZIP64, descriptor,
    // encryption, symlink, prefix, trailing payload or extraction path.
    const qsizetype end = bytes.lastIndexOf(QByteArray("PK\x05\x06", 4));
    if (end < 0 || !span(bytes, end, 22) || end + 22 + u16(bytes, end + 20) != bytes.size() ||
        u16(bytes, end + 20) > 1024 || u16(bytes, end + 4) || u16(bytes, end + 6) ||
        u16(bytes, end + 8) != 1 || u16(bytes, end + 10) != 1) return false;
    const quint32 centralSize = u32(bytes, end + 12), central = u32(bytes, end + 16);
    if (!span(bytes, central, centralSize) || quint64(central) + centralSize != quint64(end) || centralSize < 46 ||
        u32(bytes, central) != 0x02014b50) return false;
    const quint16 flags = u16(bytes, central + 8), method = u16(bytes, central + 10);
    const quint32 crc = u32(bytes, central + 16), packed = u32(bytes, central + 20), size = u32(bytes, central + 24);
    const quint16 nameLength = u16(bytes, central + 28), extraLength = u16(bytes, central + 30), commentLength = u16(bytes, central + 32);
    const quint32 mode = u32(bytes, central + 38) >> 16;
    if (u16(bytes, central + 6) > 20 || (flags & ~0x0806) || (method != 0 && method != 8) ||
        u16(bytes, central + 34) != 0 || u32(bytes, central + 42) != 0 ||
        ((mode & 0170000) && (mode & 0170000) != 0100000) || !size || size > MaxExpanded ||
        !packed || packed > MaxCompressed || centralSize != 46u + nameLength + extraLength + commentLength ||
        bytes.mid(central + 46, nameLength) != expected) return false;
    if (u32(bytes, 0) != 0x04034b50 || u16(bytes, 4) > 20 || u16(bytes, 6) != flags || u16(bytes, 8) != method ||
        u32(bytes, 14) != crc || u32(bytes, 18) != packed || u32(bytes, 22) != size ||
        u16(bytes, 26) != nameLength || !span(bytes, 30, quint64(nameLength) + u16(bytes, 28)) ||
        bytes.mid(30, nameLength) != expected) return false;
    const quint64 offset = 30u + nameLength + u16(bytes, 28);
    if (!span(bytes, offset, packed) || offset + packed != central) return false;
    if (method == 0) {
        if (packed != size) return false;
        expanded = bytes.mid(qsizetype(offset), packed);
    } else {
        expanded.resize(size);
        z_stream stream{};
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(bytes.constData() + offset));
        stream.avail_in = packed;
        stream.next_out = reinterpret_cast<Bytef*>(expanded.data()); stream.avail_out = size;
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return false;
        const int result = inflate(&stream, Z_FINISH);
        const bool valid = result == Z_STREAM_END && stream.total_in == packed && stream.total_out == size;
        inflateEnd(&stream);
        if (!valid) return false;
    }
    return quint32(crc32(0, reinterpret_cast<const Bytef*>(expanded.constData()), uInt(expanded.size()))) == crc;
}
}

QString KisMasterCatalog::sourceUrl(const QString& market) {
    if (!QStringList{"KOSPI", "KOSDAQ", "KONEX"}.contains(market)) return {};
    return "https://new.real.download.dws.co.kr/common/master/" + market.toLower() + "_code.mst.zip";
}
QList<MarketInstrument> KisMasterCatalog::parseZip(const QByteArray& bytes, const QString& market, QString* error) {
    if (error) error->clear();
    auto fail = [&]() -> QList<MarketInstrument> {
        if (error) *error = QStringLiteral("한국투자 공식 종목 파일의 압축·문자·필드 형식을 확인하지 못했습니다.");
        return {};
    };
    if (sourceUrl(market).isEmpty()) return fail();
    QByteArray raw;
    if (!unzip(bytes, market.toLower().toLatin1() + "_code.mst", raw)) return fail();
    const auto rows = raw.split('\n');
    if (rows.size() < 2 || rows.size() > 20001 || !rows.last().isEmpty()) return fail();
    const int tailLength = market == "KOSPI" ? 227 : market == "KOSDAQ" ? 221 : 184;
    const int haltOffset = market == "KOSPI" ? 60 : market == "KOSDAQ" ? 55 : 21;
    const QRegularExpression code("\\A[A-Z0-9]{6,9}\\z"), isin("\\A[A-Z0-9]{12}\\z"), groupPattern("\\A[A-Z]{2}\\z");
    QSet<QString> symbols;
    QList<MarketInstrument> result; result.reserve(rows.size() - 1);
    for (qsizetype i = 0; i + 1 < rows.size(); ++i) {
        auto row = rows[i]; if (row.endsWith('\r')) row.chop(1);
        if (row.size() < 22 + tailLength || row.size() > 1024) return fail();
        QString line;
        if (!decodeCp949(row, line) || line.size() < 22 + tailLength) return fail();
        for (QChar c : line) if (c.isNull() || c.category() == QChar::Other_Control || c.category() == QChar::Other_Format) return fail();
        const QString tail = line.right(tailLength);
        for (QChar c : tail) if (c.unicode() < 32 || c.unicode() > 126) return fail();
        const QString symbol = line.left(9).trimmed(), standard = line.mid(9, 12), name = line.mid(21, line.size() - 21 - tailLength).trimmed();
        const QString group = tail.left(2), flags = tail.mid(haltOffset, 3), warning = tail.mid(haltOffset + 3, 2);
        if (!code.match(symbol).hasMatch() || !isin.match(standard).hasMatch() || name.isEmpty() || name.size() > 120 ||
            !groupPattern.match(group).hasMatch() || symbols.contains(symbol) ||
            (flags[0] != 'Y' && flags[0] != 'N') || (flags[1] != 'Y' && flags[1] != 'N') ||
            (flags[2] != 'Y' && flags[2] != 'N')) return fail();
        symbols.insert(symbol);
        MarketInstrument item;
        item.symbol = item.baseAsset = symbol; item.name = name; item.quoteAsset = "KRW"; item.market = market;
        item.securityType = group;
        if (market != "KONEX") {
            // Official KOSPI/KOSDAQ master fixed-width fields. Preserve raw
            // values, including a blank ETP code; unknown enums are not stocks
            // by inference and do not invalidate unrelated catalog rows.
            item.preferredClass = tail.mid(market == "KOSPI" ? 158 : 153, 1);
            item.etpClass = tail.mid(market == "KOSPI" ? 22 : 18, 1);
        }
        item.tradable = QStringList{"ST", "EF", "EN"}.contains(group) && flags[0] == 'N' && flags[1] == 'N';
        item.status = flags[0] == 'Y' ? "HALTED" : flags[1] == 'Y' ? "LIQUIDATION" : "LISTED";
        QStringList notes{"증권그룹 " + group};
        if (!QStringList{"ST", "EF", "EN"}.contains(group)) notes.append(QStringLiteral("%1 신규 주문 범위 미구현").arg(productName()));
        if (flags[0] == 'Y') notes.append("거래정지");
        if (flags[1] == 'Y') notes.append(QStringLiteral("정리매매 · %1 신규 주문 범위 미구현").arg(productName()));
        if (flags[2] == 'Y') notes.append("관리종목");
        if (warning != "00") notes.append("시장경고 " + warning);
        item.warning = notes.join(" · ");
        result.append(std::move(item));
    }
    return result;
}
}
