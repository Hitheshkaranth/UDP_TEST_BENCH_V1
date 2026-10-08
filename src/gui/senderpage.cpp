#include "senderpage.h"

#include "common.h"
#include "frameview.h"
#include "netutil.h"
#include "payload.h"
#include "ratechart.h"
#include "udpsender.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

// Builds the whole tab: settings and data-field boxes, results, buttons, chart / packet
// structure, table and log. All UDP work is delegated to a UdpSender (core library).
SenderPage::SenderPage(QWidget *parent)
    : QWidget(parent)
{
    auto *leftColumn = new QVBoxLayout;
    leftColumn->addWidget(createSettingsBox());
    leftColumn->addWidget(createDataFieldBox());

    auto *topRow = new QHBoxLayout;
    topRow->addLayout(leftColumn, 1);
    topRow->addWidget(createResultsBox(), 1);

    // Controls
    m_startBtn = new QPushButton(tr("Start test"));
    m_startBtn->setDefault(true);
    m_stopBtn = new QPushButton(tr("Stop"));
    m_exportBtn = new QPushButton(tr("Export CSV..."));
    m_progress = new QProgressBar;
    m_progress->setFormat(tr("%v / %m s"));
    m_progress->setValue(0);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_startBtn);
    buttons->addWidget(m_stopBtn);
    buttons->addWidget(m_progress, 1);
    buttons->addWidget(m_exportBtn);

    // Chart, table, log
    m_chart = new RateChart;
    m_txSeries = m_chart->addSeries(tr("Sent"), QColor(0x2f, 0x7e, 0xd8));
    m_rxSeries = m_chart->addSeries(tr("Received"), QColor(0x22, 0xa0, 0x5a));

    m_table = makeStatsTable({tr("Time (s)"), tr("Tx (Mbps)"), tr("Rx (Mbps)"), tr("Loss (%)"),
                              tr("Jitter (ms)"), tr("Data errors"), tr("Send errors")});
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);

    auto *bottom = new QSplitter(Qt::Horizontal);
    bottom->addWidget(m_table);
    bottom->addWidget(m_log);
    bottom->setStretchFactor(0, 3);
    bottom->setStretchFactor(1, 2);

    m_frameView = new FrameView;
    m_viewTabs = new QTabWidget;
    m_viewTabs->addTab(m_chart, tr("Throughput"));
    m_viewTabs->addTab(m_frameView, tr("Packet structure"));

    auto *split = new QSplitter(Qt::Vertical);
    split->addWidget(m_viewTabs);
    split->addWidget(bottom);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(topRow);
    layout->addLayout(buttons);
    layout->addWidget(split, 1);

    // UDP engine (core library, independent of the GUI)
    m_sender = new UdpSender(this);
    connect(m_sender, &UdpSender::logMessage, this, &SenderPage::log);
    connect(m_sender, &UdpSender::txStats, this, &SenderPage::onTxStats);
    connect(m_sender, &UdpSender::remoteStats, this, &SenderPage::onRemoteStats);
    connect(m_sender, &UdpSender::sampleDatagram, this, &SenderPage::onSampleDatagram);
    connect(m_sender, &UdpSender::finished, this, &SenderPage::onFinished);

    connect(m_port, QOverload<int>::of(&QSpinBox::valueChanged), this, &SenderPage::updatePayloadPreview);

    // The progress bar always shows the selected test duration ("0 / 10 s" before a test)
    m_progress->setRange(0, m_duration->value());
    connect(m_duration, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int seconds) {
        if (!m_running) {
            m_progress->setRange(0, seconds);
            m_progress->setValue(0);
        }
    });
    connect(m_startBtn, &QPushButton::clicked, this, &SenderPage::start);
    connect(m_stopBtn, &QPushButton::clicked, this, &SenderPage::stop);
    connect(m_exportBtn, &QPushButton::clicked, this, [this] {
        exportTableCsv(this, m_table,
                       QStringLiteral("udp_sender_%1.csv")
                               .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    });

    onPatternChanged();   // sets the hint and builds the initial preview
    setRunning(false);
}

// Creates the "Test settings" box: receiver address, port, rate, duration.
QWidget *SenderPage::createSettingsBox()
{
    m_target = new QLineEdit(QStringLiteral("127.0.0.1"));
    m_target->setPlaceholderText(tr("Receiver IP address or host name"));

    m_port = new QSpinBox;
    m_port->setRange(1, 65535);
    m_port->setValue(Proto::kDefaultPort);

    m_rate = new QDoubleSpinBox;
    m_rate->setRange(0.01, 100000.0);
    m_rate->setDecimals(2);
    m_rate->setValue(100.0);
    m_rate->setSuffix(tr(" Mbps"));
    m_unlimited = new QCheckBox(tr("Unlimited (as fast as possible)"));
    connect(m_unlimited, &QCheckBox::toggled, m_rate, &QWidget::setDisabled);

    m_duration = new QSpinBox;
    m_duration->setRange(1, 24 * 3600);
    m_duration->setValue(10);
    m_duration->setSuffix(tr(" s"));

    auto *rateRow = new QHBoxLayout;
    rateRow->addWidget(m_rate);
    rateRow->addWidget(m_unlimited);
    rateRow->addStretch();

    auto *form = new QFormLayout;
    form->addRow(tr("Receiver address:"), m_target);
    form->addRow(tr("Port:"), m_port);
    form->addRow(tr("Target rate:"), rateRow);
    form->addRow(tr("Duration:"), m_duration);
    auto *box = new QGroupBox(tr("Test settings"));
    box->setLayout(form);
    return box;
}

// Creates the "Data field" box: size, pattern, value, validated hex preview and error injection.
QWidget *SenderPage::createDataFieldBox()
{
    m_dataSize = new QSpinBox;
    m_dataSize->setRange(Proto::kMinDataSize, Proto::kMaxDataSize);
    m_dataSize->setValue(Proto::kDefaultDataSize);
    m_dataSize->setSuffix(tr(" bytes"));
    m_dataSize->setToolTip(tr("Number of data bytes in each datagram (%1 to %2).\n"
                              "Each datagram adds %3 bytes around it: FE FA + header and stop byte 33.\n"
                              "1447 gives a 1472-byte datagram, the largest that fits a 1500-byte MTU "
                              "without IP fragmentation; 8947 does the same for 9000-byte jumbo frames.")
                                   .arg(Proto::kMinDataSize)
                                   .arg(Proto::kMaxDataSize)
                                   .arg(Proto::kFramingOverhead));
    connect(m_dataSize, QOverload<int>::of(&QSpinBox::valueChanged), this, &SenderPage::updatePayloadPreview);

    m_pattern = new QComboBox;
    for (PayloadPattern p : kAllPayloadPatterns)
        m_pattern->addItem(Payload::patternName(p), int(p));
    connect(m_pattern, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SenderPage::onPatternChanged);

    m_patternValue = new QLineEdit;
    connect(m_patternValue, &QLineEdit::textChanged, this, &SenderPage::updatePayloadPreview);

    m_payloadPreview = new QLabel;
    m_payloadPreview->setWordWrap(true);
    m_payloadPreview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_payloadPreview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    m_corruptEvery = new QSpinBox;
    m_corruptEvery->setRange(0, 1000000);
    m_corruptEvery->setValue(0);
    m_corruptEvery->setSpecialValueText(tr("Off"));
    m_corruptEvery->setPrefix(tr("1 in "));
    m_corruptEvery->setToolTip(tr("Error injection for testing the receiver's validation: flips one "
                                  "data-field byte in every Nth datagram."));

    auto *form = new QFormLayout;
    form->addRow(tr("Size:"), m_dataSize);
    form->addRow(tr("Pattern:"), m_pattern);
    form->addRow(tr("Value:"), m_patternValue);
    form->addRow(tr("Preview:"), m_payloadPreview);
    form->addRow(tr("Corrupt datagrams:"), m_corruptEvery);
    auto *box = new QGroupBox(tr("Data field (between header and stop byte 33)"));
    box->setLayout(form);
    return box;
}

// Creates the "Results" box with the live value labels.
QWidget *SenderPage::createResultsBox()
{
    m_txRateLbl = makeValueLabel();
    m_rxRateLbl = makeValueLabel();
    m_lossLbl = makeValueLabel();
    m_jitterLbl = makeValueLabel();
    m_sentLbl = makeValueLabel();
    m_errorsLbl = makeValueLabel();
    m_frameErrLbl = makeValueLabel();
    m_dataErrLbl = makeValueLabel();

    auto *grid = new QGridLayout;
    int row = 0;
    auto addStat = [&](const QString &name, QLabel *value) {
        grid->addWidget(new QLabel(name), row, 0);
        grid->addWidget(value, row, 1);
        ++row;
    };
    addStat(tr("Send rate:"), m_txRateLbl);
    addStat(tr("Receive rate (at receiver):"), m_rxRateLbl);
    addStat(tr("Datagram loss:"), m_lossLbl);
    addStat(tr("Jitter:"), m_jitterLbl);
    addStat(tr("Data-field errors:"), m_dataErrLbl);
    addStat(tr("Framing errors (FE FA / 33):"), m_frameErrLbl);
    addStat(tr("Sent:"), m_sentLbl);
    addStat(tr("Send errors:"), m_errorsLbl);
    grid->setColumnStretch(1, 1);
    grid->setRowStretch(row, 1);
    auto *box = new QGroupBox(tr("Results"));
    box->setLayout(grid);
    return box;
}

// Updates the value placeholder for the selected pattern and rebuilds the preview.
void SenderPage::onPatternChanged()
{
    const auto pattern = PayloadPattern(m_pattern->currentData().toInt());
    m_patternValue->setPlaceholderText(Payload::valueHint(pattern));
    m_patternValue->setToolTip(Payload::valueHint(pattern));
    updatePayloadPreview();
}

// Validates the data-field input against the current datagram size; shows either the
// resulting bytes or the reason the input is invalid (in red), and enables Start accordingly.
void SenderPage::updatePayloadPreview()
{
    PayloadConfig cfg;
    cfg.pattern = PayloadPattern(m_pattern->currentData().toInt());
    cfg.value = m_patternValue->text();

    const int length = m_dataSize->value();
    QString error;
    m_payloadValid = Payload::build(cfg, length, m_payload, &error);
    if (m_payloadValid) {
        const QString value = cfg.value.trimmed();
        m_payloadDescription = value.isEmpty() ? Payload::patternName(cfg.pattern)
                                               : QStringLiteral("%1 \"%2\"").arg(Payload::patternName(cfg.pattern), value);
        m_payloadPreview->setStyleSheet(QString());
        m_payloadPreview->setText(tr("%1 bytes (%2-byte datagram): %3")
                                          .arg(length)
                                          .arg(Proto::packetSizeFor(length))
                                          .arg(Payload::hexPreview(m_payload, 16)));
    } else {
        m_payload.clear();
        m_payloadPreview->setStyleSheet(QStringLiteral("color: #d03030;"));
        m_payloadPreview->setText(tr("Invalid: %1").arg(error));
    }
    updateStartEnabled();

    // Packet structure preview with the current settings (not while a test shows its real datagram)
    if (m_frameView && !m_running) {
        if (m_payloadValid) {
            m_frameView->showDatagram(tr("Preview: data datagram with the current settings"),
                                      Proto::buildDatagram(Proto::Data, 0, 0, 0, m_payload), -1, m_port->value(),
                                      true);
        } else {
            m_frameView->showMessage(tr("The data field is invalid: %1").arg(error));
        }
    }
}

// Shows the first data datagram actually sent in the running test.
void SenderPage::onSampleDatagram(const QByteArray &datagram, quint16 localPort)
{
    m_frameView->showDatagram(tr("First data datagram sent in this test"), datagram, localPort, m_port->value(),
                              false);
}

// Start is possible only when no test is running and the data field is valid.
void SenderPage::updateStartEnabled()
{
    if (m_startBtn)
        m_startBtn->setEnabled(!m_running && m_payloadValid);
}

// Enables/disables the controls for the running and idle states.
void SenderPage::setRunning(bool running)
{
    m_running = running;
    m_stopBtn->setEnabled(running);
    const QList<QWidget *> inputs{m_target, m_port, m_dataSize, m_unlimited, m_duration,
                                  m_pattern, m_patternValue, m_corruptEvery};
    for (QWidget *w : inputs)
        w->setEnabled(!running);
    m_rate->setEnabled(!running && !m_unlimited->isChecked());
    updateStartEnabled();
}

// Clears chart, table and value labels before a new test.
void SenderPage::resetResults()
{
    m_chart->clear();
    m_table->setRowCount(0);
    for (QLabel *l : {m_txRateLbl, m_rxRateLbl, m_lossLbl, m_jitterLbl, m_sentLbl, m_errorsLbl,
                      m_frameErrLbl, m_dataErrLbl})
        l->setText(QStringLiteral("–"));
    m_remoteFresh = false;
    m_haveTxFinal = false;
}

// Appends a time-stamped line to the log pane.
void SenderPage::log(const QString &text)
{
    m_log->appendPlainText(QStringLiteral("[%1] %2")
                                   .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), text));
}

// Validates the inputs, resolves the receiver address and starts the test through UdpSender.
void SenderPage::start()
{
    if (m_running)
        return;
    updatePayloadPreview();
    if (!m_payloadValid) {
        QMessageBox::warning(this, tr("Invalid data field"), m_payloadPreview->text());
        return;
    }

    const QString host = m_target->text().trimmed();
    const QHostAddress addr = resolveHost(host);
    if (addr.isNull()) {
        QMessageBox::warning(this, tr("Invalid address"), tr("Cannot resolve \"%1\".").arg(host));
        return;
    }

    SenderConfig cfg;
    cfg.target = addr;
    cfg.port = quint16(m_port->value());
    cfg.packetSize = Proto::packetSizeFor(m_dataSize->value());
    cfg.rateMbps = m_rate->value();
    cfg.unlimited = m_unlimited->isChecked();
    cfg.durationSec = m_duration->value();
    cfg.payload = m_payload;
    cfg.payloadDescription = m_payloadDescription;
    cfg.corruptEvery = m_corruptEvery->value();

    resetResults();
    m_chart->setWindowSeconds(std::min(cfg.durationSec + 2, 120));
    m_progress->setRange(0, cfg.durationSec);
    m_progress->setValue(0);

    setRunning(true);
    m_runClock.start();
    m_sender->start(cfg);
}

// Ends the data phase early (the receiver's final report is still collected).
void SenderPage::stop()
{
    m_sender->stop();
    m_stopBtn->setEnabled(false);
    log(tr("Stopping..."));
}

// The test has finished; return to the idle state.
void SenderPage::onFinished()
{
    setRunning(false);
    log(tr("Test finished."));
}

// Shows the sender's per-second statistics, or the summary when s.final is set.
void SenderPage::onTxStats(const TxStats &s)
{
    m_errorsLbl->setText(QString::number(s.sendErrors));
    m_sentLbl->setText(tr("%1 datagrams / %2").arg(s.totalPackets).arg(formatBytes(s.totalBytes)));

    if (s.final) {
        m_txFinal = s;
        m_haveTxFinal = true;
        m_progress->setValue(m_progress->maximum());
        m_txRateLbl->setText(tr("%1 (average)").arg(formatRate(s.avgMbps())));
        QString line = tr("Sender: %1 datagrams (%2) in %3 s, average %4, send errors %5")
                               .arg(s.totalPackets)
                               .arg(formatBytes(s.totalBytes))
                               .arg(s.elapsedSec, 0, 'f', 2)
                               .arg(formatRate(s.avgMbps()))
                               .arg(s.sendErrors);
        if (s.corruptedPackets > 0)
            line += tr(", deliberately corrupted %1").arg(s.corruptedPackets);
        log(line + QLatin1Char('.'));
        return;
    }

    m_progress->setValue(int(s.elapsedSec));
    m_txRateLbl->setText(formatRate(s.intervalMbps()));
    m_chart->append(m_txSeries, s.elapsedSec, s.intervalMbps());

    QStringList cells{QString::number(s.elapsedSec, 'f', 1), QString::number(s.intervalMbps(), 'f', 2)};
    if (m_remoteFresh) {
        cells << QString::number(m_lastRemote.intervalMbps(), 'f', 2)
              << QString::number(m_lastRemote.intervalLossPct(), 'f', 3)
              << QString::number(m_lastRemote.jitterMs, 'f', 3)
              << QString::number(m_lastRemote.payloadErrors);
        m_remoteFresh = false;
    } else {
        cells << QStringLiteral("–") << QStringLiteral("–") << QStringLiteral("–") << QStringLiteral("–");
    }
    cells << QString::number(s.sendErrors);
    appendTableRow(m_table, cells);
}

// Shows the receiver's reported statistics; on the final report, logs the summary
// and a PASS/FAIL verdict.
void SenderPage::onRemoteStats(const RxStats &s)
{
    const QString loss = tr("%1 of %2 (%3 %)")
                                 .arg(s.totalLost)
                                 .arg(s.totalPackets + s.totalLost)
                                 .arg(s.totalLossPct(), 0, 'f', 3);
    m_lossLbl->setText(loss);
    m_jitterLbl->setText(tr("%1 ms").arg(s.jitterMs, 0, 'f', 3));
    m_dataErrLbl->setText(QString::number(s.payloadErrors));
    m_frameErrLbl->setText(QString::number(s.framingErrors));

    if (!s.final) {
        m_lastRemote = s;
        m_remoteFresh = true;
        m_rxRateLbl->setText(formatRate(s.intervalMbps()));
        m_chart->append(m_rxSeries, m_runClock.nsecsElapsed() / 1e9, s.intervalMbps());
        return;
    }

    m_rxRateLbl->setText(tr("%1 (average)").arg(formatRate(s.avgMbps())));
    log(tr("Receiver: %1 datagrams (%2) in %3 s, average %4, lost %5, out of order %6, jitter %7 ms, "
           "data-field errors %8, framing errors %9.")
                .arg(s.totalPackets)
                .arg(formatBytes(s.totalBytes))
                .arg(s.elapsedSec, 0, 'f', 2)
                .arg(formatRate(s.avgMbps()))
                .arg(loss)
                .arg(s.outOfOrder)
                .arg(s.jitterMs, 0, 'f', 3)
                .arg(s.payloadErrors)
                .arg(s.framingErrors));
    if (m_haveTxFinal && m_txFinal.totalPackets > 0) {
        log(tr("Delivered %1 % of sent datagrams; achieved throughput %2.")
                    .arg(100.0 * double(s.totalPackets) / double(m_txFinal.totalPackets), 0, 'f', 3)
                    .arg(formatRate(s.avgMbps())));
    }
    const bool pass = s.totalLost == 0 && s.payloadErrors == 0 && s.framingErrors == 0;
    log(pass ? tr("RESULT: PASS (no loss, all frames and data fields valid)")
             : tr("RESULT: FAIL (lost %1, data-field errors %2, framing errors %3)")
                       .arg(s.totalLost)
                       .arg(s.payloadErrors)
                       .arg(s.framingErrors));
}
