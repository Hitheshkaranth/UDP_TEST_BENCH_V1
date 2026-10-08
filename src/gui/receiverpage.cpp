#include "receiverpage.h"

#include "common.h"
#include "frameview.h"
#include "netutil.h"
#include "ratechart.h"
#include "udpreceiver.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

// Builds the tab. All UDP work is delegated to a UdpReceiver (core library); the socket
// opens on "Start listening".
ReceiverPage::ReceiverPage(QWidget *parent)
    : QWidget(parent)
{
    auto *topRow = new QHBoxLayout;
    topRow->addWidget(createListenerBox(), 1);
    topRow->addWidget(createResultsBox(), 1);

    // Controls
    m_listenBtn = new QPushButton(tr("Start listening"));
    m_stopBtn = new QPushButton(tr("Stop"));
    m_exportBtn = new QPushButton(tr("Export CSV..."));
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_listenBtn);
    buttons->addWidget(m_stopBtn);
    buttons->addStretch();
    buttons->addWidget(m_exportBtn);

    // Chart, table, log
    m_chart = new RateChart;
    m_rxSeries = m_chart->addSeries(tr("Received"), QColor(0x22, 0xa0, 0x5a));

    m_table = makeStatsTable({tr("Time (s)"), tr("Rx (Mbps)"), tr("Datagrams"), tr("Lost"),
                              tr("Loss (%)"), tr("Jitter (ms)"), tr("Data errors"), tr("Frame errors")});
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);

    auto *bottom = new QSplitter(Qt::Horizontal);
    bottom->addWidget(m_table);
    bottom->addWidget(m_log);
    bottom->setStretchFactor(0, 3);
    bottom->setStretchFactor(1, 2);

    m_frameView = new FrameView;
    m_frameView->showMessage(tr("No datagram received yet. Start listening, then run a test from the sender."));
    auto *viewTabs = new QTabWidget;
    viewTabs->addTab(m_chart, tr("Throughput"));
    viewTabs->addTab(m_frameView, tr("Packet structure"));

    auto *split = new QSplitter(Qt::Vertical);
    split->addWidget(viewTabs);
    split->addWidget(bottom);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(topRow);
    layout->addLayout(buttons);
    layout->addWidget(split, 1);

    // UDP engine (core library, independent of the GUI)
    m_receiver = new UdpReceiver(this);
    connect(m_receiver, &UdpReceiver::listening, this, &ReceiverPage::onListening);
    connect(m_receiver, &UdpReceiver::stopped, this, &ReceiverPage::onStopped);
    connect(m_receiver, &UdpReceiver::sessionStarted, this, &ReceiverPage::onSessionStarted);
    connect(m_receiver, &UdpReceiver::intervalStats, this, &ReceiverPage::onInterval);
    connect(m_receiver, &UdpReceiver::sessionFinished, this, &ReceiverPage::onSessionFinished);
    connect(m_receiver, &UdpReceiver::logMessage, this, &ReceiverPage::log);
    connect(m_receiver, &UdpReceiver::sampleDatagram, this, &ReceiverPage::onSampleDatagram);

    connect(m_listenBtn, &QPushButton::clicked, this, &ReceiverPage::startListening);
    connect(m_stopBtn, &QPushButton::clicked, this, &ReceiverPage::stopListening);
    connect(m_exportBtn, &QPushButton::clicked, this, [this] {
        exportTableCsv(this, m_table,
                       QStringLiteral("udp_receiver_%1.csv")
                               .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    });

    setListening(false);
}

// Closes the socket (sending a final report if a test is running) before the page goes away.
ReceiverPage::~ReceiverPage()
{
    m_receiver->shutdown();
}

// Creates the "Listener" box: bind address, port, validation option, local IPs, status.
QWidget *ReceiverPage::createListenerBox()
{
    m_bindAddr = new QComboBox;
    m_bindAddr->setEditable(true);
    m_bindAddr->addItem(QStringLiteral("0.0.0.0"));
    m_bindAddr->addItems(localIPv4Addresses(true));
    const QStringList localIps = localIPv4Addresses(false);
    m_bindAddr->setToolTip(tr("0.0.0.0 listens on all network interfaces."));

    m_port = new QSpinBox;
    m_port->setRange(1, 65535);
    m_port->setValue(Proto::kDefaultPort);

    m_validate = new QCheckBox(tr("Validate data field of every datagram"));
    m_validate->setChecked(true);
    m_validate->setToolTip(tr("Compares each datagram's data field with the reference the sender "
                              "transmits in its start datagram. Framing (FE FA ... 33) is always checked."));

    auto *ipsLbl = new QLabel(localIps.isEmpty() ? tr("(none found)") : localIps.join(QStringLiteral(", ")));
    ipsLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ipsLbl->setWordWrap(true);

    m_statusLbl = new QLabel;

    auto *form = new QFormLayout;
    form->addRow(tr("Bind address:"), m_bindAddr);
    form->addRow(tr("Port:"), m_port);
    form->addRow(tr("Validation:"), m_validate);
    form->addRow(tr("This machine's IPs:"), ipsLbl);
    form->addRow(tr("Status:"), m_statusLbl);
    auto *box = new QGroupBox(tr("Listener"));
    box->setLayout(form);
    return box;
}

// Creates the "Results" box with the live value labels.
QWidget *ReceiverPage::createResultsBox()
{
    m_peerLbl = makeValueLabel();
    m_rateLbl = makeValueLabel();
    m_avgLbl = makeValueLabel();
    m_receivedLbl = makeValueLabel();
    m_lossLbl = makeValueLabel();
    m_oooLbl = makeValueLabel();
    m_jitterLbl = makeValueLabel();
    m_dataErrLbl = makeValueLabel();
    m_frameErrLbl = makeValueLabel();

    auto *grid = new QGridLayout;
    int row = 0;
    auto addStat = [&](const QString &name, QLabel *value) {
        grid->addWidget(new QLabel(name), row, 0);
        grid->addWidget(value, row, 1);
        ++row;
    };
    addStat(tr("Sender:"), m_peerLbl);
    addStat(tr("Current rate:"), m_rateLbl);
    addStat(tr("Average rate:"), m_avgLbl);
    addStat(tr("Received:"), m_receivedLbl);
    addStat(tr("Datagram loss:"), m_lossLbl);
    addStat(tr("Out of order:"), m_oooLbl);
    addStat(tr("Jitter:"), m_jitterLbl);
    addStat(tr("Data-field errors:"), m_dataErrLbl);
    addStat(tr("Framing errors (FE FA / 33):"), m_frameErrLbl);
    grid->setColumnStretch(1, 1);
    auto *box = new QGroupBox(tr("Results"));
    box->setLayout(grid);
    return box;
}

// Enables/disables the controls for the listening and stopped states.
void ReceiverPage::setListening(bool listening)
{
    m_listenBtn->setEnabled(!listening);
    m_stopBtn->setEnabled(listening);
    m_bindAddr->setEnabled(!listening);
    m_port->setEnabled(!listening);
    m_validate->setEnabled(!listening);
    m_statusLbl->setText(listening ? tr("Listening") : tr("Stopped"));
}

// Appends a time-stamped line to the log pane.
void ReceiverPage::log(const QString &text)
{
    m_log->appendPlainText(QStringLiteral("[%1] %2")
                                   .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), text));
}

// Opens the UDP socket with the current settings (through UdpReceiver).
void ReceiverPage::startListening()
{
    const QString addr = m_bindAddr->currentText().trimmed();
    const quint16 port = quint16(m_port->value());
    const bool validate = m_validate->isChecked();
    m_listenBtn->setEnabled(false);
    m_receiver->startListening(addr, port, validate);
}

// Closes the socket (ending any running session).
void ReceiverPage::stopListening()
{
    m_stopBtn->setEnabled(false);
    m_receiver->stopListening();
}

// Shows the first data datagram of the current test in the "Packet structure" tab.
void ReceiverPage::onSampleDatagram(const QByteArray &datagram, quint16 srcPort, quint16 dstPort)
{
    m_frameView->showDatagram(tr("First data datagram received in this test"), datagram, srcPort, dstPort, false);
}

// Result of startListening(): logs it and shows a message box on failure.
void ReceiverPage::onListening(bool ok, const QString &message)
{
    log(message);
    setListening(ok);
    if (!ok)
        QMessageBox::warning(this, tr("Receiver"), message);
}

// The socket has been closed.
void ReceiverPage::onStopped()
{
    log(tr("Listener stopped."));
    setListening(false);
}

// A sender started a test: clears the previous results.
void ReceiverPage::onSessionStarted(const QString &peer, const QString &details)
{
    m_chart->clear();
    m_chart->setWindowSeconds(10);
    m_table->setRowCount(0);
    for (QLabel *l : {m_rateLbl, m_avgLbl, m_receivedLbl, m_lossLbl, m_oooLbl, m_jitterLbl,
                      m_dataErrLbl, m_frameErrLbl})
        l->setText(QStringLiteral("–"));
    m_peerLbl->setText(peer);
    m_frameView->showMessage(tr("Waiting for the first data datagram from %1 ...").arg(peer));
    m_statusLbl->setText(tr("Receiving test traffic"));
    log(tr("Test started by %1: %2").arg(peer, details));
}

// Updates the cumulative value labels from a statistics snapshot.
void ReceiverPage::showTotals(const RxStats &s)
{
    m_avgLbl->setText(formatRate(s.avgMbps()));
    m_receivedLbl->setText(tr("%1 datagrams / %2").arg(s.totalPackets).arg(formatBytes(s.totalBytes)));
    m_lossLbl->setText(tr("%1 of %2 (%3 %)")
                               .arg(s.totalLost)
                               .arg(s.totalPackets + s.totalLost)
                               .arg(s.totalLossPct(), 0, 'f', 3));
    m_oooLbl->setText(QString::number(s.outOfOrder));
    m_jitterLbl->setText(tr("%1 ms").arg(s.jitterMs, 0, 'f', 3));
    m_dataErrLbl->setText(QString::number(s.payloadErrors));
    m_frameErrLbl->setText(QString::number(s.framingErrors));
}

// One-second interval: updates labels, chart and table.
void ReceiverPage::onInterval(const RxStats &s)
{
    showTotals(s);
    m_rateLbl->setText(formatRate(s.intervalMbps()));
    // The test length is unknown here, so the visible time span grows with the test (10-120 s).
    m_chart->setWindowSeconds(std::clamp(std::ceil(s.elapsedSec) + 2.0, 10.0, 120.0));
    m_chart->append(m_rxSeries, s.elapsedSec, s.intervalMbps());
    appendTableRow(m_table, {QString::number(s.elapsedSec, 'f', 1),
                             QString::number(s.intervalMbps(), 'f', 2),
                             QString::number(s.intervalPackets),
                             QString::number(std::max<qint64>(0, s.intervalLost)),
                             QString::number(s.intervalLossPct(), 'f', 3),
                             QString::number(s.jitterMs, 'f', 3),
                             QString::number(s.payloadErrors),
                             QString::number(s.framingErrors)});
}

// The test ended: shows the totals and logs the summary with a PASS/FAIL verdict.
void ReceiverPage::onSessionFinished(const RxStats &s, const QString &reason)
{
    showTotals(s);
    m_rateLbl->setText(QStringLiteral("–"));
    m_statusLbl->setText(m_stopBtn->isEnabled() ? tr("Listening") : tr("Stopped"));
    log(tr("Test ended (%1): %2 datagrams (%3) in %4 s, average %5, lost %6 (%7 %), "
           "out of order %8, jitter %9 ms.")
                .arg(reason)
                .arg(s.totalPackets)
                .arg(formatBytes(s.totalBytes))
                .arg(s.elapsedSec, 0, 'f', 2)
                .arg(formatRate(s.avgMbps()))
                .arg(s.totalLost)
                .arg(s.totalLossPct(), 0, 'f', 3)
                .arg(s.outOfOrder)
                .arg(s.jitterMs, 0, 'f', 3));
    const bool pass = s.totalLost == 0 && s.payloadErrors == 0 && s.framingErrors == 0;
    log(pass ? tr("RESULT: PASS (no loss, all frames and data fields valid)")
             : tr("RESULT: FAIL (lost %1, data-field errors %2, framing errors %3)")
                       .arg(s.totalLost)
                       .arg(s.payloadErrors)
                       .arg(s.framingErrors));
}
