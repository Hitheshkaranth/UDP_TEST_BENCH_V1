#pragma once

// Wire protocol shared by sender and receiver, plus the host-side statistics
// structures passed between worker threads and the GUI.
//
// Every datagram (both directions) is framed as:
//
//   +------+------+------+-------+---------+-----+--------+----------------+------+
//   | 0xFE | 0xFA | type | flags | session | seq | sendNs |   data field   | 0x33 |
//   +------+------+------+-------+---------+-----+--------+----------------+------+
//     start bytes  \___________ Header (24 bytes) ________/                  stop byte
//
// Data datagrams carry the user-selected data field. The Start datagram carries
// StartBody followed by a copy of that data field, which the receiver keeps as
// the reference for validating every data datagram.
//
// Multi-byte fields are little-endian (Q*_le types), so the tool works between
// machines of different endianness. Datagrams that do not start with FE FA and
// end with 33 are rejected and counted as framing errors.

#include <QByteArray>
#include <QMetaType>
#include <QtEndian>
#include <algorithm>
#include <cstring>

namespace Proto {

constexpr quint8 kStartByte0 = 0xFE;
constexpr quint8 kStartByte1 = 0xFA;
constexpr quint8 kStopByte = 0x33;
constexpr int kTrailerSize = 1;          // the stop byte

constexpr quint16 kDefaultPort = 5201;

enum Type : quint8 {
    Data = 1,     // test traffic, sender -> receiver
    Start = 2,    // session announce, sender -> receiver (StartBody + data field)
    Stop = 3,     // end of test, sender -> receiver (seq = total datagrams sent)
    Report = 4,   // statistics, receiver -> sender (ReportBody)
};

enum Flags : quint8 {
    FinalReport = 0x01,
};

#pragma pack(push, 1)
struct Header {
    quint8 start[2];     // kStartByte0, kStartByte1
    quint8 type;
    quint8 flags;
    quint32_le session;
    quint64_le seq;
    qint64_le sendNs;    // sender's monotonic clock, nanoseconds
};

struct StartBody {
    quint32_le packetSize;   // size of each data datagram, start/stop bytes included
    quint64_le rateBps;      // 0 = unlimited
    quint32_le durationMs;
};

struct ReportBody {
    quint64_le totalPackets;
    quint64_le totalBytes;
    quint64_le totalLost;
    quint64_le outOfOrder;
    quint64_le framingErrors;
    quint64_le payloadErrors;
    qint64_le jitterNs;
    qint64_le elapsedNs;
    quint64_le intervalPackets;
    quint64_le intervalBytes;
    qint64_le intervalLost;
    qint64_le intervalNs;
};
#pragma pack(pop)

static_assert(sizeof(Header) == 24, "unexpected Header size");
static_assert(sizeof(StartBody) == 16, "unexpected StartBody size");
static_assert(sizeof(ReportBody) == 96, "unexpected ReportBody size");

constexpr int kHeaderSize = int(sizeof(Header));
constexpr int kStartBodySize = int(sizeof(StartBody));
constexpr int kReportSize = kHeaderSize + int(sizeof(ReportBody)) + kTrailerSize;
constexpr int kStopSize = kHeaderSize + kTrailerSize;

// Bytes a datagram adds around its data field: FE FA + rest of header, and stop byte 33.
constexpr int kFramingOverhead = kHeaderSize + kTrailerSize;

// Datagram size limits (start/stop bytes included). The smallest datagram carries a
// 1-byte data field. The upper limit leaves room for the Start datagram, which is
// StartBody bigger than a data datagram and must still fit the largest IPv4 UDP
// payload (65507 bytes).
constexpr int kMinPacketSize = kFramingOverhead + 1;
constexpr int kMaxPacketSize = 65507 - kStartBodySize;

// Number of data-field bytes in a datagram of the given total size.
constexpr int payloadSize(int packetSize)
{
    return packetSize - kFramingOverhead;
}

// Total datagram size needed for a data field of the given size.
constexpr int packetSizeFor(int dataSize)
{
    return dataSize + kFramingOverhead;
}

// Data-field size limits, as entered by the user.
constexpr int kMinDataSize = payloadSize(kMinPacketSize);   // 1 byte
constexpr int kMaxDataSize = payloadSize(kMaxPacketSize);
constexpr int kDefaultDataSize = 1447;                      // 1472-byte datagram, fits a 1500-byte MTU

// Serialises a header (start bytes included) into the first kHeaderSize bytes of buf.
inline void writeHeader(char *buf, Type type, quint32 session, quint64 seq, qint64 sendNs,
                        quint8 flags = 0)
{
    Header h;
    h.start[0] = kStartByte0;
    h.start[1] = kStartByte1;
    h.type = type;
    h.flags = flags;
    h.session = session;
    h.seq = seq;
    h.sendNs = sendNs;
    std::memcpy(buf, &h, sizeof h);
}

// Puts the stop byte in the last position of a datagram of len bytes.
inline void writeTrailer(char *buf, qint64 len)
{
    buf[len - 1] = char(kStopByte);
}

// Builds a complete framed datagram: FE FA header, `body` (StartBody/ReportBody bytes or
// the data field) and the 33 stop byte. All datagrams are built through this function.
inline QByteArray buildDatagram(Type type, quint32 session, quint64 seq, qint64 sendNs, const QByteArray &body,
                                quint8 flags = 0)
{
    QByteArray out(kHeaderSize + body.size() + kTrailerSize, Qt::Uninitialized);
    writeHeader(out.data(), type, session, seq, sendNs, flags);
    if (!body.isEmpty())
        std::memcpy(out.data() + kHeaderSize, body.constData(), size_t(body.size()));
    writeTrailer(out.data(), out.size());
    return out;
}

// True when the datagram is long enough and has the FE FA start bytes and the 33 stop byte.
inline bool hasValidFraming(const char *buf, qint64 len)
{
    return len >= qint64(kHeaderSize) + kTrailerSize
        && quint8(buf[0]) == kStartByte0
        && quint8(buf[1]) == kStartByte1
        && quint8(buf[len - 1]) == kStopByte;
}

// Checks the framing and, if valid, copies the header out of the datagram.
inline bool readHeader(const char *buf, qint64 len, Header &h)
{
    if (!hasValidFraming(buf, len))
        return false;
    std::memcpy(&h, buf, sizeof h);
    return true;
}

} // namespace Proto

// One sender-side measurement interval (or the whole run when final == true).
struct TxStats
{
    bool final = false;
    double elapsedSec = 0;   // since the first data datagram
    double intervalSec = 0;
    quint64 intervalPackets = 0;
    quint64 intervalBytes = 0;
    quint64 totalPackets = 0;
    quint64 totalBytes = 0;
    quint64 sendErrors = 0;
    quint64 corruptedPackets = 0;   // datagrams deliberately corrupted (error injection)

    // Send rate during this interval, in Mbit/s.
    double intervalMbps() const { return intervalSec > 0 ? intervalBytes * 8.0 / intervalSec / 1e6 : 0.0; }
    // Average send rate since the start of the test, in Mbit/s.
    double avgMbps() const { return elapsedSec > 0 ? totalBytes * 8.0 / elapsedSec / 1e6 : 0.0; }
};

// One receiver-side measurement interval (or the whole session when final == true).
struct RxStats
{
    quint32 session = 0;
    bool final = false;
    double elapsedSec = 0;       // since the first data datagram of the session
    quint64 totalPackets = 0;
    quint64 totalBytes = 0;
    quint64 totalLost = 0;
    quint64 outOfOrder = 0;
    quint64 framingErrors = 0;   // datagrams rejected for bad start/stop bytes
    quint64 payloadErrors = 0;   // datagrams whose data field did not match the reference
    double jitterMs = 0;         // RFC 3550 interarrival jitter
    double intervalSec = 0;
    quint64 intervalPackets = 0;
    quint64 intervalBytes = 0;
    qint64 intervalLost = 0;     // can be negative when late datagrams fill earlier gaps

    // Receive rate during this interval, in Mbit/s.
    double intervalMbps() const { return intervalSec > 0 ? intervalBytes * 8.0 / intervalSec / 1e6 : 0.0; }
    // Average receive rate over the session, in Mbit/s.
    double avgMbps() const { return elapsedSec > 0 ? totalBytes * 8.0 / elapsedSec / 1e6 : 0.0; }

    // Percentage of datagrams lost over the whole session.
    double totalLossPct() const
    {
        const double expected = double(totalPackets + totalLost);
        return expected > 0 ? 100.0 * double(totalLost) / expected : 0.0;
    }

    // Percentage of datagrams lost during this interval.
    double intervalLossPct() const
    {
        const double lost = double(std::max<qint64>(0, intervalLost));
        const double expected = double(intervalPackets) + lost;
        return expected > 0 ? 100.0 * lost / expected : 0.0;
    }
};

Q_DECLARE_METATYPE(TxStats)
Q_DECLARE_METATYPE(RxStats)

namespace Proto {

// Builds a complete, framed Report datagram from receiver statistics.
inline QByteArray encodeReport(const RxStats &s)
{
    ReportBody b;
    b.totalPackets = s.totalPackets;
    b.totalBytes = s.totalBytes;
    b.totalLost = s.totalLost;
    b.outOfOrder = s.outOfOrder;
    b.framingErrors = s.framingErrors;
    b.payloadErrors = s.payloadErrors;
    b.jitterNs = qint64(s.jitterMs * 1e6);
    b.elapsedNs = qint64(s.elapsedSec * 1e9);
    b.intervalPackets = s.intervalPackets;
    b.intervalBytes = s.intervalBytes;
    b.intervalLost = s.intervalLost;
    b.intervalNs = qint64(s.intervalSec * 1e9);
    const QByteArray body(reinterpret_cast<const char *>(&b), int(sizeof b));
    return buildDatagram(Report, s.session, 0, 0, body, s.final ? FinalReport : 0);
}

// Parses a Report datagram; returns false if it is not a valid, framed report.
inline bool decodeReport(const char *buf, qint64 len, RxStats &s)
{
    Header h;
    if (!readHeader(buf, len, h) || h.type != Report || len < kReportSize)
        return false;
    ReportBody b;
    std::memcpy(&b, buf + kHeaderSize, sizeof b);
    s.session = h.session;
    s.final = (h.flags & FinalReport) != 0;
    s.totalPackets = b.totalPackets;
    s.totalBytes = b.totalBytes;
    s.totalLost = b.totalLost;
    s.outOfOrder = b.outOfOrder;
    s.framingErrors = b.framingErrors;
    s.payloadErrors = b.payloadErrors;
    s.jitterMs = double(qint64(b.jitterNs)) / 1e6;
    s.elapsedSec = double(qint64(b.elapsedNs)) / 1e9;
    s.intervalPackets = b.intervalPackets;
    s.intervalBytes = b.intervalBytes;
    s.intervalLost = b.intervalLost;
    s.intervalSec = double(qint64(b.intervalNs)) / 1e9;
    return true;
}

} // namespace Proto
