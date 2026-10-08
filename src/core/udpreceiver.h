#pragma once

#include "protocol.h"

#include <QByteArray>
#include <QObject>

class QThread;
class ReceiverWorker;

// GUI-independent API for the receiving side. Owns a worker thread for its whole
// lifetime; all signals are delivered in the thread that owns this object, so callers
// never deal with sockets or threads.
//
//   UdpReceiver receiver;
//   connect(&receiver, &UdpReceiver::sessionFinished, ...);
//   receiver.startListening("0.0.0.0", 5201, true);
class UdpReceiver : public QObject
{
    Q_OBJECT
public:
    explicit UdpReceiver(QObject *parent = nullptr);
    ~UdpReceiver() override;

    // Opens the UDP socket; the result arrives through listening(ok, message).
    void startListening(const QString &bindAddress, quint16 port, bool validatePayload);

    // Closes the socket (a running test is finished and reported first); emits stopped().
    void stopListening();

    // Synchronously closes the socket and ends the worker thread. Signals emitted while
    // closing (e.g. the final sessionFinished) are still queued for this object's thread.
    void shutdown();

signals:
    void listening(bool ok, const QString &message);
    void stopped();
    void sessionStarted(const QString &peer, const QString &details);
    void intervalStats(const RxStats &stats);
    void sessionFinished(const RxStats &stats, const QString &reason);
    void logMessage(const QString &text);
    void sampleDatagram(const QByteArray &datagram, quint16 srcPort, quint16 dstPort);

private:
    QThread *m_thread = nullptr;
    ReceiverWorker *m_worker = nullptr;
};
