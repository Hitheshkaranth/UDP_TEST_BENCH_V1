#include "receiverworker.h"

#include "format.h"
#include "payload.h"

#include <QTimer>
#include <QUdpSocket>
#include <QVariant>
#include <cmath>

namespace {
constexpr qint64 kNsPerMs = 1000000LL;
constexpr qint64 kNsPerSec = 1000000000LL;
constexpr qint64 kIdleTimeoutNs = 5 * kNsPerSec;
constexpr int kRecvBufferBytes = 8 * 1024 * 1024;
constexpr int kReadBudget = 20000;     // datagrams per readyRead before yielding to the event loop
constexpr int kMaxErrorLogs = 10;      // detailed error messages per session

// Formats "address:port".
QString endpoint(const QHostAddress &addr, quint16 port)
{
    return QStringLiteral("%1:%2").arg(addr.toString()).arg(port);
}

// Formats a session id as 8 hex digits.
QString sessionName(quint32 session)
{
    return QStringLiteral("%1").arg(session, 8, 16, QLatin1Char('0'));
}

// Formats one byte as two upper-case hex digits.
QString hexByte(char c)
{
    return QStringLiteral("%1").arg(uint(quint8(c)), 2, 16, QLatin1Char('0')).toUpper();
}
} // namespace

// Creates the worker; the socket is only opened by startListening().
ReceiverWorker::ReceiverWorker(QObject *parent)
    : QObject(parent)
    , m_buffer(65536, Qt::Uninitialized)
{
    m_clock.start();
}

// Binds the UDP socket on bindAddress:port, enlarges its receive buffer and starts the
// housekeeping timer. Emits listening(ok, message) with the result.
void ReceiverWorker::startListening(const QString &bindAddress, quint16 port, bool validatePayload)
{
    closeSocket();
    m_haveSession = false;
    m_active = false;
    m_validate = validatePayload;

    QHostAddress addr;
    if (!addr.setAddress(bindAddress.trimmed())) {
        emit listening(false, tr("Invalid bind address \"%1\".").arg(bindAddress));
        return;
    }

    m_socket = new QUdpSocket(this);
    if (!m_socket->bind(addr, port)) {
        const QString err = m_socket->errorString();
        delete m_socket;
        m_socket = nullptr;
        emit listening(false, tr("Cannot bind %1: %2").arg(endpoint(addr, port), err));
        return;
    }
    m_socket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, kRecvBufferBytes);
    const int actualBuf = m_socket->socketOption(QAbstractSocket::ReceiveBufferSizeSocketOption).toInt();
    connect(m_socket, &QUdpSocket::readyRead, this, &ReceiverWorker::onReadyRead);

    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setInterval(250);
        connect(m_timer, &QTimer::timeout, this, &ReceiverWorker::onTick);
    }
    m_timer->start();

    emit listening(true, tr("Listening on %1 (socket receive buffer %2 KiB, data-field validation %3)")
                                 .arg(endpoint(addr, port))
                                 .arg(actualBuf / 1024)
                                 .arg(m_validate ? tr("on") : tr("off")));
}

// Ends any running session (which sends the final report) and closes the socket.
void ReceiverWorker::stopListening()
{
    if (m_active)
        finishSession(tr("listener stopped"));
    closeSocket();
    emit stopped();
}

// Stops the timer and destroys the socket, if open.
void ReceiverWorker::closeSocket()
{
    if (m_timer)
        m_timer->stop();
    if (m_socket) {
        m_socket->close();
        delete m_socket;
        m_socket = nullptr;
    }
}

// Drains pending datagrams, dispatching each one by type. Datagrams without the
// FE FA start bytes or the 33 stop byte are counted as framing errors.
void ReceiverWorker::onReadyRead()
{
    if (!m_socket)
        return;

    char *buf = m_buffer.data();
    int budget = kReadBudget;
    while (budget > 0 && m_socket && m_socket->hasPendingDatagrams()) {
        --budget;
        QHostAddress from;
        quint16 fromPort = 0;
        const qint64 n = m_socket->readDatagram(buf, m_buffer.size(), &from, &fromPort);
        if (n < 0)
            break;
        const qint64 now = m_clock.nsecsElapsed();

        Proto::Header h;
        if (!Proto::readHeader(buf, n, h)) {
            handleBadFrame(buf, n);
            continue;
        }
        switch (h.type) {
        case Proto::Data:
            handleData(h, buf, n, from, fromPort, now);
            break;
        case Proto::Start:
            handleStart(h, buf, n, from, fromPort, now);
            break;
        case Proto::Stop:
            handleStop(h);
            break;
        default:
            break;
        }

        if (m_active && m_firstNs >= 0 && now - m_ivStartNs >= kNsPerSec)
            emitInterval(now);
    }

    // Under heavy load, let timers and other events run, then continue draining.
    if (budget == 0 && m_socket && m_socket->hasPendingDatagrams())
        QMetaObject::invokeMethod(this, &ReceiverWorker::onReadyRead, Qt::QueuedConnection);
}

// Called 4x per second: closes idle sessions and keeps per-second reports flowing even
// when no traffic arrives (so a dead link shows up as 0 Mbps).
void ReceiverWorker::onTick()
{
    if (!m_active)
        return;
    const qint64 now = m_clock.nsecsElapsed();
    const qint64 lastActivity = m_firstNs >= 0 ? m_lastNs : m_sessionStartNs;
    if (now - lastActivity > kIdleTimeoutNs) {
        finishSession(tr("timed out, no datagrams for %1 s").arg(kIdleTimeoutNs / kNsPerSec));
        return;
    }
    if (m_firstNs >= 0 && now - m_ivStartNs >= kNsPerSec)
        emitInterval(now);
}

// Resets all counters for a new test session from the given sender.
void ReceiverWorker::beginSession(quint32 session, const QHostAddress &from, quint16 fromPort, qint64 now)
{
    if (m_haveSession)
        m_prevSession = m_session;
    m_haveSession = true;
    m_active = true;
    m_session = session;
    m_peer = from;
    m_peerPort = fromPort;
    m_sessionStartNs = now;
    m_firstNs = -1;
    m_lastNs = now;

    m_expectedPayload.clear();
    m_haveReference = false;

    m_packets = 0;
    m_bytes = 0;
    m_outOfOrder = 0;
    m_framingErrors = 0;
    m_payloadErrors = 0;
    m_errorsLogged = 0;
    m_maxSeq = -1;
    m_announcedTotal = 0;
    m_jitterNs = 0;
    m_prevTransit = 0;
    m_haveTransit = false;

    m_ivStartNs = now;
    m_ivPackets = 0;
    m_ivBytes = 0;
    m_ivExpectedStart = 0;
}

// Counts (during a session) and logs a datagram whose framing is wrong.
void ReceiverWorker::handleBadFrame(const char *buf, qint64 len)
{
    if (!m_active)
        return;   // stray traffic between tests is ignored
    ++m_framingErrors;
    if (m_errorsLogged >= kMaxErrorLogs)
        return;
    ++m_errorsLogged;
    QString detail;
    if (len < qint64(Proto::kHeaderSize) + Proto::kTrailerSize) {
        detail = tr("only %1 bytes long").arg(len);
    } else {
        detail = tr("starts with %1 %2 (expected FE FA), ends with %3 (expected 33)")
                         .arg(hexByte(buf[0]), hexByte(buf[1]), hexByte(buf[len - 1]));
    }
    emit logMessage(tr("Framing error: %1-byte datagram %2.").arg(len).arg(detail));
    if (m_errorsLogged == kMaxErrorLogs)
        emit logMessage(tr("Further framing/data errors are counted but not logged."));
}

// Start datagram: begins a new session and stores the data-field reference that
// follows the StartBody.
void ReceiverWorker::handleStart(const Proto::Header &h, const char *buf, qint64 len,
                                 const QHostAddress &from, quint16 fromPort, qint64 now)
{
    const quint32 session = h.session;
    if (m_haveSession && (session == m_session || session == m_prevSession))
        return;   // duplicate announce
    if (m_active)
        finishSession(tr("superseded by a new session"));
    beginSession(session, from, fromPort, now);

    QString details = tr("session %1").arg(sessionName(session));
    const qint64 fixedPart = Proto::kHeaderSize + Proto::kStartBodySize + Proto::kTrailerSize;
    if (len >= fixedPart) {
        Proto::StartBody body;
        std::memcpy(&body, buf + Proto::kHeaderSize, sizeof body);
        const int packetSize = int(quint32(body.packetSize));
        const quint64 rateBps = body.rateBps;
        details += tr(", %1-byte datagrams at %2 for %3 s")
                           .arg(packetSize)
                           .arg(rateBps ? formatRate(double(rateBps) / 1e6) : tr("maximum rate"))
                           .arg(double(quint32(body.durationMs)) / 1000.0);

        const qint64 refLen = len - fixedPart;
        if (refLen > 0 && packetSize >= Proto::kMinPacketSize && refLen == Proto::payloadSize(packetSize)) {
            m_expectedPayload = QByteArray(buf + Proto::kHeaderSize + Proto::kStartBodySize, int(refLen));
            m_haveReference = true;
        }
    }

    if (!m_validate)
        details += tr(", data-field validation off");
    else if (m_haveReference)
        details += tr(", validating %1-byte data field [%2]")
                           .arg(m_expectedPayload.size())
                           .arg(Payload::hexPreview(m_expectedPayload, 8));
    else
        details += tr(", data-field validation unavailable (no reference in start datagram)");

    emit sessionStarted(endpoint(from, fromPort), details);
}

// Data datagram: updates counters, loss/out-of-order tracking, jitter and validates
// the data field. Starts a session implicitly if the Start datagram was missed.
void ReceiverWorker::handleData(const Proto::Header &h, const char *buf, qint64 len,
                                const QHostAddress &from, quint16 fromPort, qint64 now)
{
    const quint32 session = h.session;
    if (!m_haveSession || session != m_session) {
        if (m_haveSession && session == m_prevSession)
            return;   // straggler from an earlier test
        if (m_active)
            finishSession(tr("superseded by a new session"));
        beginSession(session, from, fromPort, now);
        emit sessionStarted(endpoint(from, fromPort),
                            tr("session %1 (start datagram not received, data-field validation unavailable)")
                                    .arg(sessionName(session)));
    } else if (!m_active) {
        return;   // late datagram after the session was closed
    }

    if (m_firstNs < 0) {
        m_firstNs = now;
        m_ivStartNs = now;
    }
    m_lastNs = now;

    ++m_packets;
    if (m_packets == 1)
        emit sampleDatagram(QByteArray(buf, int(len)), fromPort, m_socket ? m_socket->localPort() : 0);
    m_bytes += quint64(len);
    ++m_ivPackets;
    m_ivBytes += quint64(len);

    const qint64 seq = qint64(quint64(h.seq));
    if (seq > m_maxSeq)
        m_maxSeq = seq;
    else
        ++m_outOfOrder;

    // RFC 3550 interarrival jitter. The clock offset between the hosts cancels out
    // because only differences of transit times are used.
    const qint64 transit = now - qint64(h.sendNs);
    if (m_haveTransit) {
        const double d = std::fabs(double(transit - m_prevTransit));
        m_jitterNs += (d - m_jitterNs) / 16.0;
    }
    m_prevTransit = transit;
    m_haveTransit = true;

    if (m_validate && m_haveReference)
        validatePayload(quint64(seq), buf, len);
}

// Compares a data datagram's length and data field with the reference from the Start
// datagram; counts and (up to kMaxErrorLogs per session) logs any mismatch.
void ReceiverWorker::validatePayload(quint64 seq, const char *buf, qint64 len)
{
    const int refLen = m_expectedPayload.size();
    const qint64 expectedLen = qint64(Proto::kHeaderSize) + refLen + Proto::kTrailerSize;

    QString problem;
    if (len != expectedLen) {
        problem = tr("length %1 bytes, expected %2").arg(len).arg(expectedLen);
    } else {
        const char *got = buf + Proto::kHeaderSize;
        const char *ref = m_expectedPayload.constData();
        if (std::memcmp(got, ref, size_t(refLen)) == 0)
            return;   // the normal, fast path
        int first = -1;
        int differing = 0;
        for (int i = 0; i < refLen; ++i) {
            if (got[i] != ref[i]) {
                if (first < 0)
                    first = i;
                ++differing;
            }
        }
        problem = tr("%1 byte(s) differ, first at data offset %2: expected %3, got %4")
                          .arg(differing)
                          .arg(first)
                          .arg(hexByte(ref[first]), hexByte(got[first]));
    }

    ++m_payloadErrors;
    if (m_errorsLogged >= kMaxErrorLogs)
        return;
    ++m_errorsLogged;
    emit logMessage(tr("Data error in datagram #%1: %2.").arg(seq).arg(problem));
    if (m_errorsLogged == kMaxErrorLogs)
        emit logMessage(tr("Further framing/data errors are counted but not logged."));
}

// Stop datagram: records how many datagrams the sender sent and closes the session.
// If the session is already closed, repeats the final report (the sender missed it).
void ReceiverWorker::handleStop(const Proto::Header &h)
{
    const quint32 session = h.session;
    if (!m_haveSession || session != m_session)
        return;
    if (m_active) {
        m_announcedTotal = qint64(quint64(h.seq));
        finishSession(tr("sender finished"));
    } else if (m_lastFinal.session == session) {
        sendReport(m_lastFinal, 1);
    }
}

// Number of datagrams the sender has sent so far, as far as the receiver can tell.
qint64 ReceiverWorker::expectedPackets() const
{
    return std::max(m_maxSeq + 1, m_announcedTotal);
}

// Builds a statistics snapshot with the cumulative counters at time `now`.
RxStats ReceiverWorker::snapshot(qint64 now) const
{
    RxStats s;
    s.session = m_session;
    s.elapsedSec = m_firstNs >= 0 ? double(now - m_firstNs) / kNsPerSec : 0.0;
    s.totalPackets = m_packets;
    s.totalBytes = m_bytes;
    s.totalLost = quint64(std::max<qint64>(0, expectedPackets() - qint64(m_packets)));
    s.outOfOrder = m_outOfOrder;
    s.framingErrors = m_framingErrors;
    s.payloadErrors = m_payloadErrors;
    s.jitterMs = m_jitterNs / 1e6;
    return s;
}

// Publishes the interval ending at `now` (to the GUI/CLI and the sender) and starts a new one.
void ReceiverWorker::emitInterval(qint64 now)
{
    RxStats s = snapshot(now);
    s.intervalSec = double(now - m_ivStartNs) / kNsPerSec;
    s.intervalPackets = m_ivPackets;
    s.intervalBytes = m_ivBytes;
    s.intervalLost = (expectedPackets() - m_ivExpectedStart) - qint64(m_ivPackets);
    emit intervalStats(s);
    sendReport(s, 1);

    m_ivStartNs = now;
    m_ivPackets = 0;
    m_ivBytes = 0;
    m_ivExpectedStart = expectedPackets();
}

// Closes the active session: flushes the last partial interval, then sends the final
// report to the sender (3 copies, in case of loss) and emits sessionFinished.
void ReceiverWorker::finishSession(const QString &reason)
{
    if (!m_active)
        return;
    if (m_firstNs >= 0 && m_ivPackets > 0 && m_lastNs - m_ivStartNs >= 200 * kNsPerMs)
        emitInterval(m_lastNs);

    RxStats fin = snapshot(m_lastNs);
    fin.final = true;
    m_active = false;
    m_lastFinal = fin;
    sendReport(fin, 3);
    emit sessionFinished(fin, reason);
}

// Sends a framed Report datagram to the session's sender.
void ReceiverWorker::sendReport(const RxStats &stats, int copies)
{
    if (!m_socket || m_peerPort == 0)
        return;
    const QByteArray pkt = Proto::encodeReport(stats);
    for (int i = 0; i < copies; ++i)
        m_socket->writeDatagram(pkt, m_peer, m_peerPort);
}
