#pragma once

#include "protocol.h"
#include "senderworker.h"

#include <QByteArray>
#include <QObject>
#include <atomic>
#include <memory>

class QThread;

// GUI-independent API for running one sender test. Owns the worker thread; all signals
// are delivered in the thread that owns this object (the GUI thread or the CLI's main
// thread), so callers never deal with sockets or threads.
//
//   UdpSender sender;
//   connect(&sender, &UdpSender::remoteStats, ...);
//   sender.start(config);   // returns immediately; finished() is emitted at the end
class UdpSender : public QObject
{
    Q_OBJECT
public:
    explicit UdpSender(QObject *parent = nullptr);
    ~UdpSender() override;

    // True while a test is running.
    bool isRunning() const { return m_thread != nullptr; }

    // Starts a test in a background thread. Returns false if one is already running.
    bool start(const SenderConfig &config);

    // Ends the data phase early; the receiver's final report is still collected.
    void stop();

    // Ends the test immediately without waiting for the receiver's final report.
    void abort();

signals:
    void logMessage(const QString &text);
    void txStats(const TxStats &stats);       // per second, then once with final == true
    void remoteStats(const RxStats &stats);   // receiver's reports, the last with final == true
    void sampleDatagram(const QByteArray &datagram, quint16 localPort);
    void finished();

private:
    void onWorkerFinished();
    void joinThread();

    QThread *m_thread = nullptr;
    std::shared_ptr<std::atomic<int>> m_control;
};
