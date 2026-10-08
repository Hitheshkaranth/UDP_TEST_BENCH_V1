#pragma once

#include <QTextBrowser>

// "Packet structure" view: shows one datagram as a colour-coded layer diagram
// (Ethernet / IPv4 / UDP / FE FA / header / data / 33 / FCS), a field table with
// decoded values, a hex dump and the number of bytes it occupies on the wire.
class FrameView : public QTextBrowser
{
    Q_OBJECT
public:
    explicit FrameView(QWidget *parent = nullptr);

    // Shows the structure of `payload` (one UDP payload, FE FA ... 33).
    // Ports may be -1 when unknown; preview marks example header values.
    void showDatagram(const QString &title, const QByteArray &payload, int srcPort, int dstPort, bool preview);

    // Replaces the view with a short message (e.g. "waiting for the first datagram").
    void showMessage(const QString &text);
};
