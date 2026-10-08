#include "udpsender.h"

#include <QThread>

// Creates an idle sender.
UdpSender::UdpSender(QObject *parent)
    : QObject(parent)
{
}

// Aborts a running test and waits for its thread to end.
UdpSender::~UdpSender()
{
    abort();
    joinThread();
}

// Creates a SenderWorker in a new thread, forwards its signals and starts the test.
bool UdpSender::start(const SenderConfig &config)
{
    if (m_thread)
        return false;

    m_control = std::make_shared<std::atomic<int>>(SenderRun);
    auto *worker = new SenderWorker(config, m_control);
    m_thread = new QThread;
    worker->moveToThread(m_thread);

    connect(m_thread, &QThread::started, worker, &SenderWorker::run);
    connect(m_thread, &QThread::finished, worker, &QObject::deleteLater);

    // Worker -> this object: queued across threads, so receivers run in our thread.
    connect(worker, &SenderWorker::logMessage, this, &UdpSender::logMessage);
    connect(worker, &SenderWorker::txStats, this, &UdpSender::txStats);
    connect(worker, &SenderWorker::remoteStats, this, &UdpSender::remoteStats);
    connect(worker, &SenderWorker::sampleDatagram, this, &UdpSender::sampleDatagram);
    connect(worker, &SenderWorker::finished, this, &UdpSender::onWorkerFinished);

    m_thread->start(QThread::HighPriority);
    return true;
}

// Ends the data phase; the worker still waits for the receiver's final report.
void UdpSender::stop()
{
    if (m_control)
        m_control->store(SenderStop);
}

// Ends the test as soon as possible.
void UdpSender::abort()
{
    if (m_control)
        m_control->store(SenderAbort);
}

// The worker's run() has returned: shut the thread down, then report completion.
void UdpSender::onWorkerFinished()
{
    joinThread();
    emit finished();
}

// Stops the worker thread's event loop, waits for it and releases it.
void UdpSender::joinThread()
{
    if (!m_thread)
        return;
    m_thread->quit();
    m_thread->wait();
    delete m_thread;
    m_thread = nullptr;
    m_control.reset();
}
