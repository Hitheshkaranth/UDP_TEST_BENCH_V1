#pragma once

// Describes what one test datagram looks like on the wire: the 8-byte UDP header the
// operating system adds, followed by this app's frame (FE FA | header | data | 33).
// Used by the GUI's "Packet structure" view and by the CLI's --show-frame option.

#include <QByteArray>
#include <QString>
#include <QVector>

namespace Frame {

// Which part of the packet a byte belongs to.
enum class Part {
    UdpHeader,    // added by the OS: ports, length, checksum
    StartBytes,   // FE FA
    Header,       // type, flags, session, sequence number, send time
    Data,         // the user's data field
    StopByte,     // 33
};

// One field of the UDP header or of the app frame.
struct Field
{
    Part part;
    QString name;
    int offset = 0;   // counted from the first byte of the UDP header
    int size = 0;
    QString hex;      // bytes in wire order
    QString value;    // decoded value
    QString meaning;
};

// Sizes of the layers around a UDP payload on an Ethernet link.
constexpr int kUdpHeaderSize = 8;
constexpr int kIpv4HeaderSize = 20;
constexpr int kEthernetHeaderSize = 14;
constexpr int kEthernetFcsSize = 4;
constexpr int kPreambleAndGap = 20;   // 8 preamble/SFD + 12 inter-frame gap, per frame

// Byte counts for one datagram sent over Ethernet / IPv4.
struct WireSize
{
    int dataField = 0;        // user data bytes
    int udpPayload = 0;       // FE FA ... 33
    int udpDatagram = 0;      // + UDP header
    int ipPacket = 0;         // + IPv4 header
    int ethernetFrame = 0;    // + Ethernet header and FCS
    int onWire = 0;           // + preamble and inter-frame gap
    double dataEfficiency = 0; // dataField / onWire, in percent
};

// Display name of a part ("UDP header", "Start bytes", ...).
QString partName(Part part);

// Which part the byte at `offset` belongs to (offset from the start of the UDP header).
Part partAt(int offset, int payloadSize);

// Builds the 8-byte UDP header for a payload. Unknown ports (-1) and the checksum,
// which the OS computes, are written as zero.
QByteArray udpHeader(int srcPort, int dstPort, int payloadSize);

// Lists every field of the UDP header followed by the app frame in `payload`.
// srcPort/dstPort may be -1 when not known; preview marks example values (test not started).
QVector<Field> describe(const QByteArray &payload, int srcPort, int dstPort, bool preview);

// Computes the per-layer sizes of one datagram with the given UDP payload size.
WireSize wireSize(int payloadSize);

// Plain-text rendering for the CLI: field table, annotated hex dump and wire sizes.
QString toText(const QByteArray &payload, int srcPort, int dstPort, bool preview);

} // namespace Frame
