#include "udpreceiver.h"

#include "receiverworker.h"

#include <QThread>

// Starts the worker thread and forwards the worker's signals to this object.
UdpReceiver::UdpReceiver(QObject *parent)
    : QObject(parent)
{
    m_thread = new QThread;
    m_worker = new ReceiverWorker;
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    // Worker -> this object: queued across threads, so receivers run in our thread.
    connect(m_worker, &ReceiverWorker::listening, this, &UdpReceiver::listening);
    connect(m_worker, &ReceiverWorker::stopped, this, &UdpReceiver::stopped);
    connect(m_worker, &ReceiverWorker::sessionStarted, this, &UdpReceiver::sessionStarted);
    connect(m_worker, &ReceiverWorker::intervalStats, this, &UdpReceiver::intervalStats);
    connect(m_worker, &ReceiverWorker::sessionFinished, this, &UdpReceiver::sessionFinished);
    connect(m_worker, &ReceiverWorker::logMessage, this, &UdpReceiver::logMessage);
    connect(m_worker, &ReceiverWorker::sampleDatagram, this, &UdpReceiver::sampleDatagram);

    m_thread->start(QThread::HighPriority);
}

// Closes the socket and ends the worker thread.
UdpReceiver::~UdpReceiver()
{
    shutdown();
}

// Asks the worker (in its thread) to open the socket.
void UdpReceiver::startListening(const QString &bindAddress, quint16 port, bool validatePayload)
{
    if (!m_worker)
        return;
    ReceiverWorker *worker = m_worker;
    QMetaObject::invokeMethod(worker, [worker, bindAddress, port, validatePayload] {
        worker->startListening(bindAddress, port, validatePayload);
    }, Qt::QueuedConnection);
}

// Asks the worker (in its thread) to close the socket.
void UdpReceiver::stopListening()
{
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, &ReceiverWorker::stopListening, Qt::QueuedConnection);
}

// Closes the socket synchronously (sending the final report of a running test), then
// stops the worker thread. Safe to call more than once.
void UdpReceiver::shutdown()
{
    if (!m_thread)
        return;
    QMetaObject::invokeMethod(m_worker, &ReceiverWorker::stopListening, Qt::BlockingQueuedConnection);
    m_thread->quit();
    m_thread->wait();   // the worker is deleted by deleteLater as the thread finishes
    delete m_thread;
    m_thread = nullptr;
    m_worker = nullptr;
}
