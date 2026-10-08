#pragma once

#include "protocol.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QObject>

class QTimer;
class QUdpSocket;

// Receives test traffic in its own thread, checks the FE FA / 33 framing and the
// data field of every datagram, computes throughput / loss / jitter per one-second
// interval and reports them both to the front end and back to the sender.
// Front ends use UdpReceiver, which owns this worker and its thread.
class ReceiverWorker : public QObject
{
    Q_OBJECT
public:
    explicit ReceiverWorker(QObject *parent = nullptr);

public slots:
    // Binds the UDP socket; validatePayload enables data-field checking.
    void startListening(const QString &bindAddress, quint16 port, bool validatePayload);
    // Ends any running session (sending its final report) and closes the socket.
    void stopListening();

signals:
    void listening(bool ok, const QString &message);
    void stopped();
    void sessionStarted(const QString &peer, const QString &details);
    void intervalStats(const RxStats &stats);
    void sessionFinished(const RxStats &stats, const QString &reason);
    void logMessage(const QString &text);   // framing / data-field error details
    void sampleDatagram(const QByteArray &datagram, quint16 srcPort, quint16 dstPort);   // first data datagram of a session

private slots:
    void onReadyRead();
    void onTick();

private:
    void closeSocket();
    void handleBadFrame(const char *buf, qint64 len);
    void handleStart(const Proto::Header &h, const char *buf, qint64 len, const QHostAddress &from,
                     quint16 fromPort, qint64 now);
    void handleData(const Proto::Header &h, const char *buf, qint64 len, const QHostAddress &from,
                    quint16 fromPort, qint64 now);
    void handleStop(const Proto::Header &h);
    void validatePayload(quint64 seq, const char *buf, qint64 len);
    void beginSession(quint32 session, const QHostAddress &from, quint16 fromPort, qint64 now);
    void finishSession(const QString &reason);
    void emitInterval(qint64 now);
    RxStats snapshot(qint64 now) const;
    qint64 expectedPackets() const;
    void sendReport(const RxStats &stats, int copies);

    QUdpSocket *m_socket = nullptr;
    QTimer *m_timer = nullptr;
    QElapsedTimer m_clock;
    QByteArray m_buffer;
    bool m_validate = true;

    // Session state
    bool m_haveSession = false;
    bool m_active = false;
    quint32 m_session = 0;
    quint32 m_prevSession = 0;
    QHostAddress m_peer;
    quint16 m_peerPort = 0;
    qint64 m_sessionStartNs = 0;
    qint64 m_firstNs = -1;   // arrival of first data datagram, -1 = none yet
    qint64 m_lastNs = 0;

    // Data-field reference received in the Start datagram
    QByteArray m_expectedPayload;
    bool m_haveReference = false;

    // Cumulative counters
    quint64 m_packets = 0;
    quint64 m_bytes = 0;
    quint64 m_outOfOrder = 0;
    quint64 m_framingErrors = 0;
    quint64 m_payloadErrors = 0;
    int m_errorsLogged = 0;
    qint64 m_maxSeq = -1;
    qint64 m_announcedTotal = 0;   // from the sender's Stop datagram
    double m_jitterNs = 0;
    qint64 m_prevTransit = 0;
    bool m_haveTransit = false;

    // Current interval
    qint64 m_ivStartNs = 0;
    quint64 m_ivPackets = 0;
    quint64 m_ivBytes = 0;
    qint64 m_ivExpectedStart = 0;

    RxStats m_lastFinal;
};
