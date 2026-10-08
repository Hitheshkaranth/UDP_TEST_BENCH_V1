#include "framelayout.h"

#include "protocol.h"

#include <QStringList>
#include <QtEndian>
#include <algorithm>

namespace Frame {

namespace {

// Formats bytes as upper-case hex separated by spaces, e.g. "FE FA".
QString hexOf(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toHex(' ').toUpper());
}

// Formats at most `maxBytes` bytes as hex, adding " ..." when truncated.
QString hexPreview(const QByteArray &bytes, int maxBytes)
{
    QString text = hexOf(bytes.left(maxBytes));
    if (bytes.size() > maxBytes)
        text += QStringLiteral(" ...");
    return text;
}

// Name of a header "type" value.
QString typeName(quint8 type)
{
    switch (type) {
    case Proto::Data:   return QStringLiteral("Data");
    case Proto::Start:  return QStringLiteral("Start");
    case Proto::Stop:   return QStringLiteral("Stop");
    case Proto::Report: return QStringLiteral("Report");
    }
    return QStringLiteral("unknown");
}

// One-letter marker per part, used under the CLI hex dump.
char partLetter(Part part)
{
    switch (part) {
    case Part::UdpHeader:  return 'U';
    case Part::StartBytes: return 'S';
    case Part::Header:     return 'H';
    case Part::Data:       return 'D';
    case Part::StopByte:   return 'E';
    }
    return '?';
}

} // namespace

// Display name of a part.
QString partName(Part part)
{
    switch (part) {
    case Part::UdpHeader:  return QStringLiteral("UDP header (added by OS)");
    case Part::StartBytes: return QStringLiteral("Start bytes FE FA");
    case Part::Header:     return QStringLiteral("App header");
    case Part::Data:       return QStringLiteral("Data field");
    case Part::StopByte:   return QStringLiteral("Stop byte 33");
    }
    return QString();
}

// Which part the byte at `offset` (from the start of the UDP header) belongs to.
Part partAt(int offset, int payloadSize)
{
    if (offset < kUdpHeaderSize)
        return Part::UdpHeader;
    const int p = offset - kUdpHeaderSize;   // offset inside the UDP payload
    if (p < 2)
        return Part::StartBytes;
    if (p < Proto::kHeaderSize)
        return Part::Header;
    if (p >= payloadSize - Proto::kTrailerSize)
        return Part::StopByte;
    return Part::Data;
}

// Builds the 8-byte UDP header (big-endian fields, checksum left as zero).
QByteArray udpHeader(int srcPort, int dstPort, int payloadSize)
{
    QByteArray h(kUdpHeaderSize, '\0');
    uchar *p = reinterpret_cast<uchar *>(h.data());
    qToBigEndian<quint16>(quint16(std::max(0, srcPort)), p);
    qToBigEndian<quint16>(quint16(std::max(0, dstPort)), p + 2);
    qToBigEndian<quint16>(quint16(kUdpHeaderSize + payloadSize), p + 4);
    return h;
}

// Lists the fields of the UDP header and of the app frame in `payload`.
QVector<Field> describe(const QByteArray &payload, int srcPort, int dstPort, bool preview)
{
    QVector<Field> fields;
    const int n = payload.size();
    const QByteArray udp = udpHeader(srcPort, dstPort, n);

    // UDP header (network byte order, filled in by the operating system)
    fields.append({Part::UdpHeader, QStringLiteral("Source port"), 0, 2,
                   srcPort >= 0 ? hexOf(udp.mid(0, 2)) : QStringLiteral("?? ??"),
                   srcPort >= 0 ? QString::number(srcPort) : QStringLiteral("(chosen by OS)"),
                   QStringLiteral("Sender's UDP port, picked by the OS")});
    fields.append({Part::UdpHeader, QStringLiteral("Destination port"), 2, 2,
                   dstPort >= 0 ? hexOf(udp.mid(2, 2)) : QStringLiteral("?? ??"),
                   dstPort >= 0 ? QString::number(dstPort) : QStringLiteral("?"),
                   QStringLiteral("Receiver's listening port")});
    fields.append({Part::UdpHeader, QStringLiteral("Length"), 4, 2, hexOf(udp.mid(4, 2)),
                   QStringLiteral("%1").arg(kUdpHeaderSize + n),
                   QStringLiteral("UDP header + payload, in bytes")});
    fields.append({Part::UdpHeader, QStringLiteral("Checksum"), 6, 2, QStringLiteral("?? ??"),
                   QStringLiteral("(computed by OS)"),
                   QStringLiteral("Covers header and payload; added when sending")});

    if (n < Proto::kHeaderSize + Proto::kTrailerSize)
        return fields;   // not one of our frames

    const int base = kUdpHeaderSize;
    Proto::Header h;
    std::memcpy(&h, payload.constData(), sizeof h);
    const quint32 session = h.session;
    const quint64 seq = h.seq;
    const qint64 sendNs = h.sendNs;
    const int dataSize = n - Proto::kFramingOverhead;

    fields.append({Part::StartBytes, QStringLiteral("Start bytes"), base + 0, 2, hexOf(payload.mid(0, 2)),
                   QStringLiteral("FE FA"), QStringLiteral("Marks the start of every frame")});
    fields.append({Part::Header, QStringLiteral("Type"), base + 2, 1, hexOf(payload.mid(2, 1)),
                   QStringLiteral("%1 = %2").arg(quint8(h.type)).arg(typeName(h.type)),
                   QStringLiteral("1 Data, 2 Start, 3 Stop, 4 Report")});
    fields.append({Part::Header, QStringLiteral("Flags"), base + 3, 1, hexOf(payload.mid(3, 1)),
                   QString::number(quint8(h.flags)), QStringLiteral("Bit 0 = final report (Report only)")});
    fields.append({Part::Header, QStringLiteral("Session ID"), base + 4, 4, hexOf(payload.mid(4, 4)),
                   preview ? QStringLiteral("(random per test)")
                           : QStringLiteral("0x%1").arg(session, 8, 16, QLatin1Char('0')).toUpper().replace(QLatin1String("0X"), QLatin1String("0x")),
                   QStringLiteral("Identifies one test run (little-endian)")});
    fields.append({Part::Header, QStringLiteral("Sequence number"), base + 8, 8, hexOf(payload.mid(8, 8)),
                   preview ? QStringLiteral("0, 1, 2, ...") : QString::number(seq),
                   QStringLiteral("Counts datagrams; used to detect loss and reordering")});
    fields.append({Part::Header, QStringLiteral("Send time"), base + 16, 8, hexOf(payload.mid(16, 8)),
                   preview ? QStringLiteral("(sender clock)") : QStringLiteral("%1 ns").arg(sendNs),
                   QStringLiteral("Sender timestamp; used to compute jitter")});
    fields.append({Part::Data, QStringLiteral("Data field"), base + Proto::kHeaderSize, dataSize,
                   hexPreview(payload.mid(Proto::kHeaderSize, dataSize), 8),
                   QStringLiteral("%1 byte(s)").arg(dataSize),
                   QStringLiteral("Your data; checked byte-for-byte by the receiver")});
    fields.append({Part::StopByte, QStringLiteral("Stop byte"), base + n - 1, 1, hexOf(payload.right(1)),
                   QStringLiteral("33"), QStringLiteral("Marks the end of every frame")});
    return fields;
}

// Computes per-layer sizes for a UDP payload sent over Ethernet / IPv4.
WireSize wireSize(int payloadSize)
{
    WireSize w;
    w.udpPayload = payloadSize;
    w.dataField = std::max(0, payloadSize - Proto::kFramingOverhead);
    w.udpDatagram = payloadSize + kUdpHeaderSize;
    w.ipPacket = w.udpDatagram + kIpv4HeaderSize;
    w.ethernetFrame = std::max(64, w.ipPacket + kEthernetHeaderSize + kEthernetFcsSize);   // 64-byte minimum frame
    w.onWire = w.ethernetFrame + kPreambleAndGap;
    w.dataEfficiency = w.onWire > 0 ? 100.0 * w.dataField / w.onWire : 0.0;
    return w;
}

// Plain-text rendering for the CLI: field table, hex dump with part markers, wire sizes.
QString toText(const QByteArray &payload, int srcPort, int dstPort, bool preview)
{
    QStringList lines;
    lines << QStringLiteral("Packet structure (offsets from the start of the UDP header):");
    lines << QStringLiteral("  Offset Size  Field             Bytes on the wire          Value");
    for (const Field &f : describe(payload, srcPort, dstPort, preview)) {
        lines << QStringLiteral("  %1 %2  %3 %4 %5")
                         .arg(f.offset, 6)
                         .arg(f.size, 4)
                         .arg(f.name, -17)
                         .arg(f.hex, -26)
                         .arg(f.value);
    }

    // Hex dump of UDP header + payload; the line under each row marks the part of each byte:
    // U = UDP header, S = start bytes, H = app header, D = data, E = stop byte.
    QByteArray bytes = udpHeader(srcPort, dstPort, payload.size()) + payload;
    const int total = bytes.size();
    const int rows = (total + 15) / 16;
    lines << QString();
    lines << QStringLiteral("Hex dump (U = UDP header, S = start FE FA, H = header, D = data, E = stop 33):");
    for (int r = 0; r < rows; ++r) {
        if (rows > 6 && r == 4) {
            lines << QStringLiteral("  ....  (%1 more data bytes)").arg((rows - 5) * 16);
            r = rows - 2;   // continue with the last row
            continue;
        }
        QString hex, marks;
        for (int i = r * 16; i < std::min(total, r * 16 + 16); ++i) {
            const bool unknown = i < kUdpHeaderSize && ((i < 2 && srcPort < 0) || (i >= 6));
            hex += unknown ? QStringLiteral("?? ")
                           : QStringLiteral("%1 ").arg(uint(quint8(bytes.at(i))), 2, 16, QLatin1Char('0')).toUpper();
            const QChar m = QLatin1Char(partLetter(partAt(i, payload.size())));
            marks += QString(2, m) + QLatin1Char(' ');
        }
        lines << QStringLiteral("  %1  %2").arg(r * 16, 4, 16, QLatin1Char('0')).arg(hex);
        lines << QStringLiteral("        %1").arg(marks);
    }

    const WireSize w = wireSize(payload.size());
    lines << QString();
    lines << QStringLiteral("On an Ethernet link each datagram is %1 + %2 (IPv4) + %3 (UDP) + %4 (FE FA..33) + %5 (FCS) "
                            "= %6-byte frame, %7 bytes with preamble/gap.")
                     .arg(kEthernetHeaderSize).arg(kIpv4HeaderSize).arg(kUdpHeaderSize).arg(payload.size())
                     .arg(kEthernetFcsSize).arg(w.ethernetFrame).arg(w.onWire);
    lines << QStringLiteral("Data field = %1 of those %2 bytes (%3 % efficiency). UDP itself has no footer; "
                            "33 is this app's stop byte.")
                     .arg(w.dataField).arg(w.onWire).arg(w.dataEfficiency, 0, 'f', 1);
    return lines.join(QLatin1Char('\n'));
}

} // namespace Frame
