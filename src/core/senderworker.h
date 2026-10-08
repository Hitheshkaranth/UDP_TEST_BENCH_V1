#pragma once

#include "protocol.h"

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <atomic>
#include <memory>

class QUdpSocket;

struct SenderConfig
{
    QHostAddress target;
    quint16 port = Proto::kDefaultPort;
    int packetSize = Proto::packetSizeFor(Proto::kDefaultDataSize);   // whole datagram, FE FA and 33 included
    double rateMbps = 100.0;    // ignored when unlimited
    bool unlimited = false;
    int durationSec = 10;
    QByteArray payload;         // data field, exactly Proto::payloadSize(packetSize) bytes
    QString payloadDescription; // e.g. "Fixed byte A5", for the log
    int corruptEvery = 0;       // error injection: corrupt 1 of every N datagrams (0 = off)
};

// Values of the shared control flag owned by the GUI / CLI.
enum SenderControl : int {
    SenderRun = 0,
    SenderStop = 1,    // end the data phase, still collect the receiver's final report
    SenderAbort = 2,   // end immediately (application shutdown)
};

// Generates paced UDP traffic in its own thread. run() blocks until the test ends;
// the owner stops it through the shared atomic flag rather than through the event loop.
// Front ends use UdpSender, which owns this worker and its thread.
class SenderWorker : public QObject
{
    Q_OBJECT
public:
    // cfg describes the test; control is the shared stop flag (SenderControl values).
    SenderWorker(const SenderConfig &cfg, std::shared_ptr<std::atomic<int>> control);

public slots:
    // Runs the whole test: Start, paced data, Stop, wait for the final report.
    void run();

signals:
    void logMessage(const QString &text);
    void txStats(const TxStats &stats);       // once per second, then once with final == true
    void remoteStats(const RxStats &stats);   // receiver's reports, the last with final == true
    void sampleDatagram(const QByteArray &datagram, quint16 localPort);   // first data datagram, as sent
    void finished();

private:
    int control() const { return m_control->load(std::memory_order_relaxed); }
    void sendStart(QUdpSocket &sock, qint64 nowNs);
    void sendStop(QUdpSocket &sock, quint64 totalSent, qint64 nowNs);
    void pollReports(QUdpSocket &sock);

    SenderConfig m_cfg;
    std::shared_ptr<std::atomic<int>> m_control;
    quint32 m_session = 0;
    bool m_gotFinal = false;
};
