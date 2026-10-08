#include "senderworker.h"

#include "format.h"
#include "payload.h"

#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QThread>
#include <QUdpSocket>
#include <QVariant>
#include <algorithm>

namespace {
constexpr qint64 kNsPerMs = 1000000LL;
constexpr qint64 kNsPerSec = 1000000000LL;
constexpr int kSendBufferBytes = 4 * 1024 * 1024;
constexpr qint64 kFinalReportWaitNs = 3 * kNsPerSec;
} // namespace

// Stores the test configuration and picks a random, non-zero session id.
SenderWorker::SenderWorker(const SenderConfig &cfg, std::shared_ptr<std::atomic<int>> control)
    : m_cfg(cfg)
    , m_control(std::move(control))
    , m_session(QRandomGenerator::global()->generate() | 1u)
{
}

// Sends the Start datagram: test parameters plus a copy of the data field, which the
// receiver uses as the reference when validating data datagrams.
void SenderWorker::sendStart(QUdpSocket &sock, qint64 nowNs)
{
    Proto::StartBody body;
    body.packetSize = quint32(m_cfg.packetSize);
    body.rateBps = m_cfg.unlimited ? quint64(0) : quint64(m_cfg.rateMbps * 1e6);
    body.durationMs = quint32(m_cfg.durationSec) * 1000u;
    const QByteArray bodyBytes = QByteArray(reinterpret_cast<const char *>(&body), int(sizeof body)) + m_cfg.payload;
    sock.writeDatagram(Proto::buildDatagram(Proto::Start, m_session, 0, nowNs, bodyBytes), m_cfg.target, m_cfg.port);
}

// Sends the Stop datagram; its seq field carries the total number of datagrams sent so
// the receiver can also count datagrams lost at the very end of the test.
void SenderWorker::sendStop(QUdpSocket &sock, quint64 totalSent, qint64 nowNs)
{
    sock.writeDatagram(Proto::buildDatagram(Proto::Stop, m_session, totalSent, nowNs, QByteArray()), m_cfg.target,
                       m_cfg.port);
}

// Reads any pending Report datagrams from the receiver and forwards them as remoteStats.
void SenderWorker::pollReports(QUdpSocket &sock)
{
    char buf[512];
    while (sock.hasPendingDatagrams()) {
        const qint64 n = sock.readDatagram(buf, sizeof buf);
        if (n < 0)
            break;
        RxStats s;
        if (!Proto::decodeReport(buf, n, s) || s.session != m_session)
            continue;
        if (s.final) {
            if (m_gotFinal)
                continue;   // the receiver sends the final report several times
            m_gotFinal = true;
        }
        emit remoteStats(s);
    }
}

// Runs the whole test in the worker thread: opens the socket, announces the session,
// sends paced data datagrams for the configured duration, then sends Stop and waits
// for the receiver's final report.
void SenderWorker::run()
{
    const int pktSize = m_cfg.packetSize;
    const int dataLen = Proto::payloadSize(pktSize);
    if (pktSize < Proto::kMinPacketSize || pktSize > Proto::kMaxPacketSize || m_cfg.payload.size() != dataLen) {
        emit logMessage(tr("Internal error: data field is %1 bytes, expected %2.")
                                .arg(m_cfg.payload.size())
                                .arg(dataLen));
        emit finished();
        return;
    }

    QUdpSocket sock;
    const QHostAddress any = m_cfg.target.protocol() == QAbstractSocket::IPv6Protocol
            ? QHostAddress(QHostAddress::AnyIPv6)
            : QHostAddress(QHostAddress::AnyIPv4);
    if (!sock.bind(any, 0)) {
        emit logMessage(tr("Cannot open UDP socket: %1").arg(sock.errorString()));
        emit finished();
        return;
    }
    sock.setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, kSendBufferBytes);

    // Datagram template: [FE FA header][data field][33]. Only the header changes per datagram.
    QByteArray packet = Proto::buildDatagram(Proto::Data, m_session, 0, 0, m_cfg.payload);
    char *const data = packet.data();

    emit logMessage(tr("Session %1: sending %2-byte datagrams to %3:%4 at %5 for %6 s")
                            .arg(m_session, 8, 16, QLatin1Char('0'))
                            .arg(pktSize)
                            .arg(m_cfg.target.toString())
                            .arg(m_cfg.port)
                            .arg(m_cfg.unlimited ? tr("maximum rate") : formatRate(m_cfg.rateMbps))
                            .arg(m_cfg.durationSec));
    emit logMessage(tr("Frame: [FE FA] [%1 header bytes] [%2-byte data field, %3: %4] [33]")
                            .arg(Proto::kHeaderSize - 2)
                            .arg(dataLen)
                            .arg(m_cfg.payloadDescription)
                            .arg(Payload::hexPreview(m_cfg.payload, 12)));
    if (m_cfg.corruptEvery > 0)
        emit logMessage(tr("Error injection: corrupting 1 of every %1 datagrams.").arg(m_cfg.corruptEvery));

    QElapsedTimer clock;
    clock.start();

    // Announce the session so the receiver resets its counters, then give it a moment.
    for (int i = 0; i < 3; ++i)
        sendStart(sock, clock.nsecsElapsed());
    QThread::msleep(20);

    // Token-bucket pacing: credit accrues at the target rate and is capped at ~20 ms
    // worth so a scheduling hiccup cannot turn into a huge burst.
    const qint64 startNs = clock.nsecsElapsed();
    const qint64 endNs = startNs + qint64(m_cfg.durationSec) * kNsPerSec;
    const double bytesPerNs = m_cfg.rateMbps * 1e6 / 8.0 / 1e9;
    const double burstBytes = std::max(4.0 * pktSize, bytesPerNs * 20.0 * kNsPerMs);
    double tokens = pktSize;
    qint64 lastNs = startNs;

    const quint64 corruptEvery = quint64(std::max(0, m_cfg.corruptEvery));
    quint64 seq = 0;
    quint64 totalBytes = 0;
    quint64 sendErrors = 0;
    quint64 corrupted = 0;
    quint64 ivPackets = 0;
    quint64 ivBytes = 0;
    qint64 ivStartNs = startNs;
    qint64 nextPollNs = startNs;
    int hardErrors = 0;
    QString lastError;

    // Publishes the statistics of the interval that ends at `now` and starts a new one.
    auto emitInterval = [&](qint64 now) {
        TxStats s;
        s.elapsedSec = double(now - startNs) / kNsPerSec;
        s.intervalSec = double(now - ivStartNs) / kNsPerSec;
        s.intervalPackets = ivPackets;
        s.intervalBytes = ivBytes;
        s.totalPackets = seq;
        s.totalBytes = totalBytes;
        s.sendErrors = sendErrors;
        s.corruptedPackets = corrupted;
        emit txStats(s);
        ivPackets = 0;
        ivBytes = 0;
        ivStartNs = now;
    };

    while (control() == SenderRun) {
        const qint64 now = clock.nsecsElapsed();
        if (now >= endNs)
            break;

        bool canSend = m_cfg.unlimited;
        if (!canSend) {
            tokens = std::min(burstBytes, tokens + double(now - lastNs) * bytesPerNs);
            lastNs = now;
            canSend = tokens >= pktSize;
        }

        if (canSend) {
            Proto::writeHeader(data, Proto::Data, m_session, seq, now);

            // Error injection: flip one data-field byte (moving position) in every Nth datagram.
            char *corruptAt = nullptr;
            if (corruptEvery > 0 && seq % corruptEvery == corruptEvery - 1) {
                corruptAt = data + Proto::kHeaderSize + int(seq % quint64(dataLen));
                *corruptAt = char(~*corruptAt);
            }

            const qint64 n = sock.writeDatagram(data, pktSize, m_cfg.target, m_cfg.port);

            // Let the front end show the first data datagram, byte for byte as sent.
            if (n == pktSize && seq == 0)
                emit sampleDatagram(QByteArray(data, pktSize), sock.localPort());

            if (corruptAt)
                *corruptAt = char(~*corruptAt);   // restore the template

            if (n == pktSize) {
                if (corruptAt)
                    ++corrupted;
                ++seq;
                ++ivPackets;
                ivBytes += quint64(n);
                totalBytes += quint64(n);
                tokens -= pktSize;
                hardErrors = 0;
            } else {
                // TemporaryError = socket send buffer full; anything else is a real failure.
                ++sendErrors;
                if (sock.error() != QAbstractSocket::TemporaryError && ++hardErrors > 1000) {
                    emit logMessage(tr("Aborting, sending keeps failing: %1").arg(sock.errorString()));
                    break;
                }
                if (sock.errorString() != lastError) {
                    lastError = sock.errorString();
                    emit logMessage(tr("Send error: %1").arg(lastError));
                }
                QThread::yieldCurrentThread();
            }
        } else {
            const double waitNs = (pktSize - tokens) / bytesPerNs;
            if (waitNs > 2.0 * kNsPerMs)
                QThread::usleep(1000);
            else
                QThread::yieldCurrentThread();
        }

        if (now >= nextPollNs) {
            pollReports(sock);
            nextPollNs = now + 5 * kNsPerMs;
        }
        if (now - ivStartNs >= kNsPerSec)
            emitInterval(now);
    }

    const qint64 dataEndNs = clock.nsecsElapsed();
    if (dataEndNs - ivStartNs >= 200 * kNsPerMs)
        emitInterval(dataEndNs);   // trailing partial interval

    TxStats total;
    total.final = true;
    total.elapsedSec = double(dataEndNs - startNs) / kNsPerSec;
    total.totalPackets = seq;
    total.totalBytes = totalBytes;
    total.sendErrors = sendErrors;
    total.corruptedPackets = corrupted;
    emit txStats(total);

    // Tell the receiver we are done and wait for its final report, repeating Stop
    // in case it gets lost.
    const qint64 waitUntil = dataEndNs + kFinalReportWaitNs;
    qint64 nextStopNs = 0;
    while (!m_gotFinal && control() != SenderAbort) {
        const qint64 now = clock.nsecsElapsed();
        if (now >= waitUntil)
            break;
        if (now >= nextStopNs) {
            sendStop(sock, seq, now);
            nextStopNs = now + 250 * kNsPerMs;
        }
        pollReports(sock);
        QThread::msleep(10);
    }

    if (!m_gotFinal && control() != SenderAbort) {
        emit logMessage(tr("No final report from the receiver. Check that it is listening on %1:%2 "
                           "and that the firewall allows UDP on that port in both directions.")
                                .arg(m_cfg.target.toString())
                                .arg(m_cfg.port));
    }
    emit finished();
}
