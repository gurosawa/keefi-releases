#pragma once
#include <QByteArray>
#include <QString>
#include <QtEndian>
#include <zlib.h>
// Synthetic master records and bounded single-entry ZIPs; no account data.
namespace keefi::test {
inline void put16(QByteArray& b, quint16 v) { char bytes[2]; qToLittleEndian(v, bytes); b.append(bytes, 2); }
inline void put32(QByteArray& b, quint32 v) { char bytes[4]; qToLittleEndian(v, bytes); b.append(bytes, 4); }
inline void replace32(QByteArray& b, qsizetype p, quint32 v) { qToLittleEndian(v, b.data() + p); }
inline QByteArray row(const QString& market, const QByteArray& symbol = "005930", const QByteArray& group = "ST", const QByteArray& flags = "NNN00") {
    const int tailSize = market == "KOSPI" ? 227 : market == "KOSDAQ" ? 221 : 184;
    const int at = market == "KOSPI" ? 60 : market == "KOSDAQ" ? 55 : 21;
    QByteArray tail(tailSize, ' '); tail.replace(0, 2, group); tail.replace(at, 5, flags);
    return symbol.leftJustified(9, ' ') + "KR7005930003" + QByteArray::fromHex("b0a1c2a5c7d1b1dbc1beb8f1") + "  " + tail + '\n';
}
inline QByteArray zip(const QByteArray& raw, const QByteArray& name = "kospi_code.mst", bool deflate = false) {
    QByteArray packed = raw;
    if (deflate) {
        packed.resize(compressBound(raw.size())); z_stream z{};
        z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(raw.constData())); z.avail_in = raw.size();
        z.next_out = reinterpret_cast<Bytef*>(packed.data()); z.avail_out = packed.size();
        if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) return {};
        const auto rc = ::deflate(&z, Z_FINISH); const auto size = z.total_out; deflateEnd(&z);
        if (rc != Z_STREAM_END) return {}; packed.resize(size);
    }
    const quint32 crc = crc32(0, reinterpret_cast<const Bytef*>(raw.constData()), raw.size());
    QByteArray b; put32(b, 0x04034b50); put16(b, 20); put16(b, 0); put16(b, deflate ? 8 : 0);
    put16(b, 0); put16(b, 0); put32(b, crc); put32(b, packed.size()); put32(b, raw.size());
    put16(b, name.size()); put16(b, 0); b += name; b += packed;
    const quint32 start = b.size();
    put32(b, 0x02014b50); put16(b, 20); put16(b, 20); put16(b, 0); put16(b, deflate ? 8 : 0);
    put16(b, 0); put16(b, 0); put32(b, crc); put32(b, packed.size()); put32(b, raw.size());
    put16(b, name.size()); put16(b, 0); put16(b, 0); put16(b, 0); put16(b, 0); put32(b, 0); put32(b, 0); b += name;
    const quint32 size = b.size() - start;
    put32(b, 0x06054b50); put16(b, 0); put16(b, 0); put16(b, 1); put16(b, 1); put32(b, size); put32(b, start); put16(b, 0);
    return b;
}
}
