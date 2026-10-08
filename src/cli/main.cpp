// udpbw-cli: command-line front end for the UDP bandwidth tester.
// Contains no networking code: all UDP work is done by UdpSender / UdpReceiver from the
// core library, the same engine the GUI uses.
// Uses the same sender/receiver engine and wire protocol as the GUI, so a CLI
// sender can test against a GUI receiver and vice versa.

#include "format.h"
#include "payload.h"
#include "protocol.h"
#include "framelayout.h"
#include "netutil.h"
#include "udpreceiver.h"
#include "udpsender.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QMutex>
#include <QTextStream>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <functional>
#include <memory>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <timeapi.h>
#endif

namespace {

enum ExitCode {
    ExitOk = 0,         // test passed: no loss, no framing or data-field errors
    ExitError = 1,      // bad arguments, socket errors
    ExitNoReport = 2,   // sender: receiver never sent its final report
    ExitFail = 3,       // test ran but found loss, framing errors or data-field errors
};

std::atomic<int> g_interrupts{0};
QMutex g_outMutex;

// SIGINT/SIGBREAK handler: only counts the interrupt; the main thread polls the counter.
void onSignal(int)
{
    g_interrupts.fetch_add(1);
    std::signal(SIGINT, onSignal);   // some platforms reset the handler after delivery
}

// Prints one line to stdout (thread-safe, flushed immediately).
void out(const QString &line)
{
    QMutexLocker lock(&g_outMutex);
    std::fputs(line.toLocal8Bit().constData(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// Prints one line to stderr (thread-safe, flushed immediately).
void err(const QString &line)
{
    QMutexLocker lock(&g_outMutex);
    std::fputs(line.toLocal8Bit().constData(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

// Optional per-interval CSV log shared by both modes.
class CsvLog
{
public:
    // Creates the file and writes the header row; an empty path disables logging.
    bool open(const QString &path)
    {
        if (path.isEmpty())
            return true;
        m_file.setFileName(path);
        if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            return false;
        m_stream.setDevice(&m_file);
        m_stream << "side,time_s,mbps,datagrams,lost,loss_pct,jitter_ms,data_errors,framing_errors\n";
        return true;
    }

    // Appends one result row (no-op when logging is disabled).
    void row(const char *side, double t, double mbps, quint64 datagrams, qint64 lost, double lossPct,
             double jitterMs, quint64 dataErrors, quint64 framingErrors)
    {
        if (!m_file.isOpen())
            return;
        m_stream << side << ',' << QString::number(t, 'f', 3) << ',' << QString::number(mbps, 'f', 3) << ','
                 << datagrams << ',' << lost << ',' << QString::number(lossPct, 'f', 4) << ','
                 << QString::number(jitterMs, 'f', 4) << ',' << dataErrors << ',' << framingErrors << '\n';
        m_stream.flush();
    }

private:
    QFile m_file;
    QTextStream m_stream;
};

// Parses a rate such as "100", "100M", "1.5G" or "500k" into Mbps (a bare number means
// Mbps); "0", "max" or "unlimited" select unlimited mode.
bool parseRate(QString text, double &mbps, bool &unlimited)
{
    text = text.trimmed().toLower();
    if (text.endsWith(QLatin1String("bps")))
        text.chop(3);
    unlimited = false;
    if (text == QLatin1String("max") || text == QLatin1String("unlimited")) {
        unlimited = true;
        return true;
    }
    double scale = 1.0;
    if (text.endsWith(QLatin1Char('k'))) {
        scale = 1e-3;
        text.chop(1);
    } else if (text.endsWith(QLatin1Char('m'))) {
        text.chop(1);
    } else if (text.endsWith(QLatin1Char('g'))) {
        scale = 1e3;
        text.chop(1);
    }
    bool ok = false;
    const double v = text.toDouble(&ok);
    if (!ok || v < 0)
        return false;
    if (v == 0) {
        unlimited = true;
        return true;
    }
    mbps = v * scale;
    return mbps >= 0.01;
}

// Formats "lost/expected (pct%)".
QString lossText(quint64 lost, quint64 expected, double pct)
{
    return QString::asprintf("%llu/%llu (%.3f%%)", static_cast<unsigned long long>(lost),
                             static_cast<unsigned long long>(expected), pct);
}

// True when the session had no loss and no framing or data-field errors.
bool passed(const RxStats &s)
{
    return s.totalLost == 0 && s.payloadErrors == 0 && s.framingErrors == 0;
}

// Prints the PASS/FAIL verdict line for a finished session.
void printVerdict(const RxStats &s)
{
    if (passed(s)) {
        out(QStringLiteral("  RESULT     : PASS (no loss, all frames and data fields valid)"));
    } else {
        out(QStringLiteral("  RESULT     : FAIL (lost %1, data-field errors %2, framing errors %3)")
                    .arg(s.totalLost).arg(s.payloadErrors).arg(s.framingErrors));
    }
}

// Polls the Ctrl+C counter from the main thread (signal handlers can't touch Qt) and
// calls onInterrupt(count) for each new interrupt.
void watchInterrupts(QObject *context, std::function<void(int)> onInterrupt)
{
    auto *timer = new QTimer(context);
    auto seen = std::make_shared<int>(0);
    QObject::connect(timer, &QTimer::timeout, context, [seen, onInterrupt] {
        const int n = g_interrupts.load();
        if (n != *seen) {
            *seen = n;
            onInterrupt(n);
        }
    });
    timer->start(100);
}

// ---------------------------------------------------------------------------
// Server (receiver)
// ---------------------------------------------------------------------------

// Runs the receiver until Ctrl+C (or after one test with --once) and prints per-second
// results and a summary for each test. All UDP work is done by UdpReceiver (core library).
int runServer(QCoreApplication &app, const QString &bindAddress, quint16 port, bool once, bool validate,
              bool showFrame, const QString &csvPath)
{
    CsvLog csv;
    if (!csv.open(csvPath)) {
        err(QStringLiteral("Cannot write CSV file %1").arg(csvPath));
        return ExitError;
    }

    UdpReceiver receiver;
    int rc = ExitOk;

    QObject::connect(&receiver, &UdpReceiver::listening, &app, [&](bool ok, const QString &msg) {
        if (ok) {
            out(msg + QStringLiteral(". Waiting for a sender, press Ctrl+C to quit."));
        } else {
            err(msg);
            rc = ExitError;
            QCoreApplication::quit();
        }
    });

    QObject::connect(&receiver, &UdpReceiver::logMessage, &app,
                     [](const QString &msg) { out(QStringLiteral("  ! ") + msg); });

    QObject::connect(&receiver, &UdpReceiver::sessionStarted, &app, [&](const QString &peer, const QString &details) {
        out(QString());
        out(QStringLiteral("Test from %1: %2").arg(peer, details));
        out(QStringLiteral("  Interval     Throughput       Datagrams   Lost/Expected (loss)        Jitter      DataErr FrameErr"));
    });

    QObject::connect(&receiver, &UdpReceiver::sampleDatagram, &app,
                     [showFrame](const QByteArray &datagram, quint16 srcPort, quint16 dstPort) {
        if (showFrame)
            out(QStringLiteral("First data datagram received:\n") + Frame::toText(datagram, srcPort, dstPort, false));
    });

    QObject::connect(&receiver, &UdpReceiver::intervalStats, &app, [&](const RxStats &s) {
        const qint64 lost = std::max<qint64>(0, s.intervalLost);
        out(QString::asprintf("  [%6.1f s]  %-15s  %10llu   %-26s  %8.3f ms  %7llu %8llu", s.elapsedSec,
                              qPrintable(formatRate(s.intervalMbps())),
                              static_cast<unsigned long long>(s.intervalPackets),
                              qPrintable(lossText(quint64(lost), s.intervalPackets + quint64(lost),
                                                  s.intervalLossPct())),
                              s.jitterMs,
                              static_cast<unsigned long long>(s.payloadErrors),
                              static_cast<unsigned long long>(s.framingErrors)));
        csv.row("rx", s.elapsedSec, s.intervalMbps(), s.intervalPackets, lost, s.intervalLossPct(), s.jitterMs,
                s.payloadErrors, s.framingErrors);
    });

    QObject::connect(&receiver, &UdpReceiver::sessionFinished, &app, [&](const RxStats &s, const QString &reason) {
        out(QStringLiteral("  ------------------------------------------------------------------------------"));
        out(QStringLiteral("  Summary (%1)").arg(reason));
        out(QStringLiteral("  Received   : %1 datagrams, %2 in %3 s")
                    .arg(s.totalPackets).arg(formatBytes(s.totalBytes)).arg(s.elapsedSec, 0, 'f', 2));
        out(QStringLiteral("  Throughput : %1").arg(formatRate(s.avgMbps())));
        out(QStringLiteral("  Lost       : %1, out of order %2, jitter %3 ms")
                    .arg(lossText(s.totalLost, s.totalPackets + s.totalLost, s.totalLossPct()))
                    .arg(s.outOfOrder).arg(s.jitterMs, 0, 'f', 3));
        out(QStringLiteral("  Integrity  : data-field errors %1, framing errors %2")
                    .arg(s.payloadErrors).arg(s.framingErrors));
        printVerdict(s);
        csv.row("rx_total", s.elapsedSec, s.avgMbps(), s.totalPackets, qint64(s.totalLost), s.totalLossPct(),
                s.jitterMs, s.payloadErrors, s.framingErrors);
        if (once) {
            rc = passed(s) ? ExitOk : ExitFail;
            QCoreApplication::quit();
        }
    });

    receiver.startListening(bindAddress, port, validate);

    watchInterrupts(&app, [](int) {
        out(QStringLiteral("Interrupted."));
        QCoreApplication::quit();
    });

    app.exec();

    // Close the socket (a running test is finished and reported), then deliver the
    // signals that produced so its summary is printed before exiting.
    receiver.shutdown();
    QCoreApplication::processEvents();
    return rc;
}

// ---------------------------------------------------------------------------
// Client (sender)
// ---------------------------------------------------------------------------

// Runs one test towards the receiver, printing the sender's and the receiver's
// per-second results and a summary with a PASS/FAIL verdict. All UDP work is done by
// UdpSender (core library).
int runClient(QCoreApplication &app, const SenderConfig &cfg, bool showFrame, const QString &csvPath)
{
    CsvLog csv;
    if (!csv.open(csvPath)) {
        err(QStringLiteral("Cannot write CSV file %1").arg(csvPath));
        return ExitError;
    }

    UdpSender sender;
    TxStats txFinal;
    RxStats rxFinal;
    bool haveTxFinal = false;
    bool haveRxFinal = false;

    QObject::connect(&sender, &UdpSender::finished, &app, &QCoreApplication::quit);
    QObject::connect(&sender, &UdpSender::logMessage, &app, [](const QString &msg) { out(msg); });

    QObject::connect(&sender, &UdpSender::sampleDatagram, &app,
                     [&cfg, showFrame](const QByteArray &datagram, quint16 localPort) {
        if (showFrame)
            out(QStringLiteral("First data datagram sent:\n") + Frame::toText(datagram, localPort, cfg.port, false));
    });

    QObject::connect(&sender, &UdpSender::txStats, &app, [&](const TxStats &s) {
        if (s.final) {
            txFinal = s;
            haveTxFinal = true;
            csv.row("tx_total", s.elapsedSec, s.avgMbps(), s.totalPackets, 0, 0, 0, 0, 0);
            return;
        }
        QString extra;
        if (s.sendErrors)
            extra += QStringLiteral("  send errors so far: %1").arg(s.sendErrors);
        if (s.corruptedPackets)
            extra += QStringLiteral("  corrupted so far: %1").arg(s.corruptedPackets);
        out(QString::asprintf("  [%6.1f s] TX  %-15s  %10llu datagrams%s", s.elapsedSec,
                              qPrintable(formatRate(s.intervalMbps())),
                              static_cast<unsigned long long>(s.intervalPackets), qPrintable(extra)));
        csv.row("tx", s.elapsedSec, s.intervalMbps(), s.intervalPackets, 0, 0, 0, 0, 0);
    });

    QObject::connect(&sender, &UdpSender::remoteStats, &app, [&](const RxStats &s) {
        if (s.final) {
            rxFinal = s;
            haveRxFinal = true;
            out(QStringLiteral("  ------------------------------------------------------------------------------"));
            if (haveTxFinal) {
                QString sent = QStringLiteral("  Sender     : %1 datagrams, %2 in %3 s -> %4")
                                       .arg(txFinal.totalPackets).arg(formatBytes(txFinal.totalBytes))
                                       .arg(txFinal.elapsedSec, 0, 'f', 2).arg(formatRate(txFinal.avgMbps()));
                if (txFinal.corruptedPackets)
                    sent += QStringLiteral(" (%1 deliberately corrupted)").arg(txFinal.corruptedPackets);
                out(sent);
            }
            out(QStringLiteral("  Receiver   : %1 datagrams, %2 in %3 s -> %4")
                        .arg(s.totalPackets).arg(formatBytes(s.totalBytes))
                        .arg(s.elapsedSec, 0, 'f', 2).arg(formatRate(s.avgMbps())));
            out(QStringLiteral("  Lost       : %1, out of order %2, jitter %3 ms")
                        .arg(lossText(s.totalLost, s.totalPackets + s.totalLost, s.totalLossPct()))
                        .arg(s.outOfOrder).arg(s.jitterMs, 0, 'f', 3));
            out(QStringLiteral("  Integrity  : data-field errors %1, framing errors %2")
                        .arg(s.payloadErrors).arg(s.framingErrors));
            printVerdict(s);
            csv.row("rx_total", s.elapsedSec, s.avgMbps(), s.totalPackets, qint64(s.totalLost), s.totalLossPct(),
                    s.jitterMs, s.payloadErrors, s.framingErrors);
            return;
        }
        const qint64 lost = std::max<qint64>(0, s.intervalLost);
        out(QString::asprintf("  [%6.1f s] RX  %-15s  %10llu datagrams  lost %s  jitter %.3f ms  data errors %llu",
                              s.elapsedSec, qPrintable(formatRate(s.intervalMbps())),
                              static_cast<unsigned long long>(s.intervalPackets),
                              qPrintable(lossText(quint64(lost), s.intervalPackets + quint64(lost), s.intervalLossPct())),
                              s.jitterMs, static_cast<unsigned long long>(s.payloadErrors)));
        csv.row("rx", s.elapsedSec, s.intervalMbps(), s.intervalPackets, lost, s.intervalLossPct(), s.jitterMs,
                s.payloadErrors, s.framingErrors);
    });

    watchInterrupts(&app, [&sender](int count) {
        if (count == 1) {
            out(QStringLiteral("Stopping (Ctrl+C again to abort without the receiver's report)..."));
            sender.stop();
        } else {
            sender.abort();
        }
    });

    sender.start(cfg);
    app.exec();

    if (!haveRxFinal)
        return ExitNoReport;
    return passed(rxFinal) ? ExitOk : ExitFail;
}

} // namespace

// Parses the command line, validates every option and runs the receiver or sender.
int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    timeBeginPeriod(1);   // 1 ms sleep granularity for the sender's rate pacing
#endif

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("udpbw-cli"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.1"));
    qRegisterMetaType<TxStats>("TxStats");
    qRegisterMetaType<RxStats>("RxStats");

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
            "UDP bandwidth tester (command line). Compatible with the GUI app.\n"
            "Datagram format: FE FA | header | data field | 33\n\n"
            "Receiver:  udpbw-cli -s [-p 5201] [-B 0.0.0.0] [--once] [--no-validate]\n"
            "Sender:    udpbw-cli -c <receiver-ip> [-b 100M] [-t 10] [-l 1447]\n"
            "                     [--pattern fixed --data A5] [--corrupt 1000]\n\n"
            "Data patterns (--pattern / --data):\n"
            "  inc     incrementing bytes; --data = first byte in hex (optional)\n"
            "  fixed   one byte repeated;  --data = byte in hex, e.g. A5\n"
            "  random  pseudo-random;      --data = seed (optional)\n"
            "  hex     custom bytes;       --data = \"DE AD BE EF\" (repeated to fill)\n"
            "  text    custom text;        --data = \"HELLO\" (repeated to fill)\n\n"
            "Exit codes: 0 = pass, 1 = error, 2 = no final report from the receiver,\n"
            "            3 = loss, framing errors or data-field errors detected."));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption serverOpt({QStringLiteral("s"), QStringLiteral("server")},
                                       QStringLiteral("Run as receiver."));
    const QCommandLineOption clientOpt({QStringLiteral("c"), QStringLiteral("client")},
                                       QStringLiteral("Run as sender towards <host>."), QStringLiteral("host"));
    const QCommandLineOption portOpt({QStringLiteral("p"), QStringLiteral("port")},
                                     QStringLiteral("UDP port (default %1).").arg(Proto::kDefaultPort),
                                     QStringLiteral("port"), QString::number(Proto::kDefaultPort));
    const QCommandLineOption bindOpt({QStringLiteral("B"), QStringLiteral("bind")},
                                     QStringLiteral("Receiver: local address to listen on (default 0.0.0.0)."),
                                     QStringLiteral("address"), QStringLiteral("0.0.0.0"));
    const QCommandLineOption onceOpt(QStringLiteral("once"),
                                     QStringLiteral("Receiver: exit after the first test completes."));
    const QCommandLineOption noValidateOpt(QStringLiteral("no-validate"),
                                           QStringLiteral("Receiver: do not check the data field "
                                                          "(framing is always checked)."));
    const QCommandLineOption rateOpt({QStringLiteral("b"), QStringLiteral("bandwidth")},
                                     QStringLiteral("Sender: target rate, e.g. 500k, 100M, 1.5G (bare number = Mbps; "
                                                    "0 or max = unlimited). Default 100M."),
                                     QStringLiteral("rate"), QStringLiteral("100M"));
    const QCommandLineOption timeOpt({QStringLiteral("t"), QStringLiteral("time")},
                                     QStringLiteral("Sender: test duration in seconds (default 10)."),
                                     QStringLiteral("seconds"), QStringLiteral("10"));
    const QCommandLineOption lenOpt({QStringLiteral("l"), QStringLiteral("length")},
                                    QStringLiteral("Sender: data-field size in bytes, %1-%2 (default %3). "
                                                   "Each datagram adds %4 bytes: FE FA + header and stop byte 33.")
                                            .arg(Proto::kMinDataSize).arg(Proto::kMaxDataSize)
                                            .arg(Proto::kDefaultDataSize).arg(Proto::kFramingOverhead),
                                    QStringLiteral("bytes"), QString::number(Proto::kDefaultDataSize));
    const QCommandLineOption patternOpt(QStringLiteral("pattern"),
                                        QStringLiteral("Sender: data-field pattern: inc, fixed, random, hex, text "
                                                       "(default inc)."),
                                        QStringLiteral("name"), QStringLiteral("inc"));
    const QCommandLineOption dataOpt(QStringLiteral("data"),
                                     QStringLiteral("Sender: pattern value (byte, seed, hex bytes or text)."),
                                     QStringLiteral("value"));
    const QCommandLineOption corruptOpt(QStringLiteral("corrupt"),
                                        QStringLiteral("Sender: corrupt one data-field byte in every Nth datagram "
                                                       "(tests the receiver's validation)."),
                                        QStringLiteral("N"), QStringLiteral("0"));
    const QCommandLineOption showFrameOpt(QStringLiteral("show-frame"),
                                          QStringLiteral("Print the structure and hex dump of the first data "
                                                         "datagram (UDP header + FE FA ... 33)."));
    const QCommandLineOption csvOpt(QStringLiteral("csv"), QStringLiteral("Write per-second results to a CSV file."),
                                    QStringLiteral("file"));
    parser.addOptions({serverOpt, clientOpt, portOpt, bindOpt, onceOpt, noValidateOpt, rateOpt, timeOpt, lenOpt,
                       patternOpt, dataOpt, corruptOpt, showFrameOpt, csvOpt});
    parser.process(app);

    std::signal(SIGINT, onSignal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, onSignal);
#endif

    bool ok = false;
    const int port = parser.value(portOpt).toInt(&ok);
    if (!ok || port < 1 || port > 65535) {
        err(QStringLiteral("Invalid port: %1").arg(parser.value(portOpt)));
        return ExitError;
    }

    int rc = ExitError;
    if (parser.isSet(serverOpt) == parser.isSet(clientOpt)) {
        err(QStringLiteral("Specify exactly one of -s (receiver) or -c <host> (sender). See --help."));
    } else if (parser.isSet(serverOpt)) {
        rc = runServer(app, parser.value(bindOpt), quint16(port), parser.isSet(onceOpt),
                       !parser.isSet(noValidateOpt), parser.isSet(showFrameOpt), parser.value(csvOpt));
    } else {
        SenderConfig cfg;
        cfg.port = quint16(port);
        const QString host = parser.value(clientOpt).trimmed();
        cfg.target = resolveHost(host);

        const int seconds = parser.value(timeOpt).toInt(&ok);
        const bool timeOk = ok && seconds >= 1 && seconds <= 24 * 3600;
        const int length = parser.value(lenOpt).toInt(&ok);
        const bool lenOk = ok && length >= Proto::kMinDataSize && length <= Proto::kMaxDataSize;
        const int corruptEvery = parser.value(corruptOpt).toInt(&ok);
        const bool corruptOk = ok && corruptEvery >= 0;

        PayloadConfig payload;
        const bool patternOk = Payload::patternFromKeyword(parser.value(patternOpt), payload.pattern);
        payload.value = parser.value(dataOpt);

        QString payloadError;
        if (cfg.target.isNull())
            err(QStringLiteral("Cannot resolve host \"%1\".").arg(host));
        else if (!parseRate(parser.value(rateOpt), cfg.rateMbps, cfg.unlimited))
            err(QStringLiteral("Invalid rate \"%1\". Examples: 500k, 100M, 1G, max.").arg(parser.value(rateOpt)));
        else if (!timeOk)
            err(QStringLiteral("Invalid duration \"%1\" (1-86400 s).").arg(parser.value(timeOpt)));
        else if (!lenOk)
            err(QStringLiteral("Invalid data-field size \"%1\" (%2-%3 bytes).")
                        .arg(parser.value(lenOpt)).arg(Proto::kMinDataSize).arg(Proto::kMaxDataSize));
        else if (!corruptOk)
            err(QStringLiteral("Invalid --corrupt value \"%1\" (0 = off, N = every Nth datagram).")
                        .arg(parser.value(corruptOpt)));
        else if (!patternOk)
            err(QStringLiteral("Unknown pattern \"%1\". Use inc, fixed, random, hex or text.")
                        .arg(parser.value(patternOpt)));
        else if (!Payload::build(payload, length, cfg.payload, &payloadError))
            err(QStringLiteral("Invalid data field (%1): %2").arg(Payload::patternKeyword(payload.pattern), payloadError));
        else {
            cfg.durationSec = seconds;
            cfg.packetSize = Proto::packetSizeFor(length);
            cfg.corruptEvery = corruptEvery;
            const QString value = payload.value.trimmed();
            cfg.payloadDescription = value.isEmpty()
                    ? Payload::patternName(payload.pattern)
                    : QStringLiteral("%1 \"%2\"").arg(Payload::patternName(payload.pattern), value);
            rc = runClient(app, cfg, parser.isSet(showFrameOpt), parser.value(csvOpt));
        }
    }

#ifdef Q_OS_WIN
    timeEndPeriod(1);
#endif
    return rc;
}
