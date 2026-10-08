#include "frameview.h"

#include "framelayout.h"
#include "protocol.h"

#include <QStringList>
#include <algorithm>

namespace {

const char *kOsColor = "#cfd4da";   // layers added by the OS / network card

// Background colour for each part (pastel; text on it is always drawn black).
QString partColor(Frame::Part part)
{
    switch (part) {
    case Frame::Part::UdpHeader:  return QStringLiteral("#9ec5fe");
    case Frame::Part::StartBytes: return QStringLiteral("#ffb86b");
    case Frame::Part::Header:     return QStringLiteral("#ffe08a");
    case Frame::Part::Data:       return QStringLiteral("#b6e3a8");
    case Frame::Part::StopByte:   return QStringLiteral("#ff9a9a");
    }
    return QString::fromLatin1(kOsColor);
}

// One coloured cell of the layer diagram: name on the first line, size on the second.
QString layerCell(const QString &name, const QString &size, const QString &bg)
{
    return QStringLiteral("<td bgcolor='%1' align='center'><font color='#000000'><b>%2</b><br/>%3</font></td>")
            .arg(bg, name.toHtmlEscaped(), size.toHtmlEscaped());
}

// Escapes text for HTML.
QString esc(const QString &s)
{
    return s.toHtmlEscaped();
}

} // namespace

// Creates an empty, read-only view.
FrameView::FrameView(QWidget *parent)
    : QTextBrowser(parent)
{
    setOpenLinks(false);
    showMessage(tr("No datagram to show yet."));
}

// Replaces the view with a short message.
void FrameView::showMessage(const QString &text)
{
    setHtml(QStringLiteral("<p><i>%1</i></p>").arg(esc(text)));
}

// Renders the layer diagram, field table, hex dump and wire-size summary for one datagram.
void FrameView::showDatagram(const QString &title, const QByteArray &payload, int srcPort, int dstPort, bool preview)
{
    const int n = payload.size();
    const Frame::WireSize w = Frame::wireSize(n);
    const int dataSize = std::max(0, n - Proto::kFramingOverhead);
    QString html;

    html += QStringLiteral("<h3 style='margin-bottom:2px'>%1</h3>").arg(esc(title));
    html += tr("<p style='margin-top:0'>UDP payload <b>%1 bytes</b> = FE FA (2) + header (22) + data (<b>%2</b>) + 33 (1). "
               "On an Ethernet link this becomes a <b>%3-byte</b> frame.</p>")
                    .arg(n).arg(dataSize).arg(w.ethernetFrame);

    // Layer diagram
    html += QStringLiteral("<table width='100%' cellspacing='2' cellpadding='5'>");
    html += QStringLiteral("<tr><td colspan='3' align='center'><small>%1</small></td>"
                           "<td colspan='4' align='center'><small><b>%2</b></small></td>"
                           "<td align='center'><small>%3</small></td></tr>")
                    .arg(tr("added by the operating system"), tr("UDP payload: sent by this app"), tr("added by NIC"));
    html += QStringLiteral("<tr>");
    html += layerCell(tr("Ethernet header"), tr("%1 B").arg(Frame::kEthernetHeaderSize), QString::fromLatin1(kOsColor));
    html += layerCell(tr("IPv4 header"), tr("%1 B").arg(Frame::kIpv4HeaderSize), QString::fromLatin1(kOsColor));
    html += layerCell(tr("UDP header"), tr("%1 B").arg(Frame::kUdpHeaderSize), partColor(Frame::Part::UdpHeader));
    html += layerCell(QStringLiteral("FE FA"), tr("2 B"), partColor(Frame::Part::StartBytes));
    html += layerCell(tr("Header"), tr("22 B"), partColor(Frame::Part::Header));
    html += layerCell(tr("Data field"), tr("%1 B").arg(dataSize), partColor(Frame::Part::Data));
    html += layerCell(QStringLiteral("33"), tr("1 B"), partColor(Frame::Part::StopByte));
    html += layerCell(tr("Ethernet FCS"), tr("%1 B").arg(Frame::kEthernetFcsSize), QString::fromLatin1(kOsColor));
    html += QStringLiteral("</tr></table>");
    html += tr("<p><small>UDP has no footer of its own: its header (8 bytes) carries ports, length and checksum. "
               "The only trailer on the wire is the 4-byte Ethernet FCS (CRC) added by the network card. "
               "<b>33</b> is this app's stop byte.</small></p>");

    // Field table
    html += QStringLiteral("<table cellspacing='0' cellpadding='3' border='1' style='border-collapse:collapse'>");
    html += tr("<tr><th></th><th>Offset</th><th>Size</th><th>Field</th><th>Bytes on the wire</th>"
               "<th>Value</th><th>Meaning</th></tr>");
    for (const Frame::Field &f : Frame::describe(payload, srcPort, dstPort, preview)) {
        html += QStringLiteral("<tr><td bgcolor='%1'>&nbsp;&nbsp;</td><td align='right'>%2</td><td align='right'>%3</td>"
                               "<td>%4</td><td><tt>%5</tt></td><td>%6</td><td><small>%7</small></td></tr>")
                        .arg(partColor(f.part))
                        .arg(f.offset)
                        .arg(f.size)
                        .arg(esc(f.name), esc(f.hex), esc(f.value), esc(f.meaning));
    }
    html += QStringLiteral("</table>");
    html += tr("<p><small>Offsets count from the first byte of the UDP header. UDP header fields are big-endian "
               "(network order); the app header fields are little-endian.</small></p>");

    // Hex dump (UDP header + payload), each byte coloured by part
    const QByteArray bytes = Frame::udpHeader(srcPort, dstPort, n) + payload;
    const int total = bytes.size();
    const int rows = (total + 15) / 16;
    html += tr("<p><b>Hex dump</b> (UDP header + payload)</p>");
    html += QStringLiteral("<pre>");
    for (int r = 0; r < rows; ++r) {
        if (rows > 8 && r == 6) {
            html += tr("....  (%1 more data bytes)\n").arg((rows - 7) * 16);
            r = rows - 2;   // continue with the last row (contains the stop byte)
            continue;
        }
        html += QStringLiteral("%1  ").arg(r * 16, 4, 16, QLatin1Char('0'));
        for (int i = r * 16; i < std::min(total, r * 16 + 16); ++i) {
            const bool unknown = i < Frame::kUdpHeaderSize && ((i < 2 && srcPort < 0) || i >= 6);
            const QString text = unknown ? QStringLiteral("??")
                                         : QStringLiteral("%1").arg(uint(quint8(bytes.at(i))), 2, 16, QLatin1Char('0')).toUpper();
            html += QStringLiteral("<span style='background-color:%1; color:#000000'>%2</span> ")
                            .arg(partColor(Frame::partAt(i, n)), text);
        }
        html += QLatin1Char('\n');
    }
    html += QStringLiteral("</pre>");

    // Wire sizes
    html += tr("<p><b>Bytes per datagram on an Ethernet link:</b> %1 (Ethernet) + %2 (IPv4) + %3 (UDP) + %4 (payload) "
               "+ %5 (FCS) = <b>%6</b>, plus %7 preamble/inter-frame gap = <b>%8 bytes</b> of link time.<br/>"
               "Your data is %9 of those bytes: <b>%10 % efficiency</b>.</p>")
                    .arg(Frame::kEthernetHeaderSize).arg(Frame::kIpv4HeaderSize).arg(Frame::kUdpHeaderSize).arg(n)
                    .arg(Frame::kEthernetFcsSize).arg(w.ethernetFrame).arg(Frame::kPreambleAndGap).arg(w.onWire)
                    .arg(w.dataField)
                    .arg(w.dataEfficiency, 0, 'f', 1);

    setHtml(html);
}
