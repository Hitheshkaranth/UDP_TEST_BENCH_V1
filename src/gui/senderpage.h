#pragma once

#include "protocol.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QWidget>

class FrameView;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class RateChart;
class UdpSender;

// "Sender" tab: test settings, data-field editor, live results, throughput chart, packet
// structure view, table and log. Contains no networking code: tests run through UdpSender.
class SenderPage : public QWidget
{
    Q_OBJECT
public:
    explicit SenderPage(QWidget *parent = nullptr);

private slots:
    void start();
    void stop();
    void onTxStats(const TxStats &s);
    void onRemoteStats(const RxStats &s);
    void onSampleDatagram(const QByteArray &datagram, quint16 localPort);
    void onFinished();
    void log(const QString &text);
    void updatePayloadPreview();
    void onPatternChanged();

private:
    QWidget *createSettingsBox();
    QWidget *createDataFieldBox();
    QWidget *createResultsBox();
    void setRunning(bool running);
    void updateStartEnabled();
    void resetResults();

    // Test settings
    QLineEdit *m_target = nullptr;
    QSpinBox *m_port = nullptr;
    QDoubleSpinBox *m_rate = nullptr;
    QCheckBox *m_unlimited = nullptr;
    QSpinBox *m_duration = nullptr;

    // Data field
    QSpinBox *m_dataSize = nullptr;   // data-field size in bytes (datagram = size + 25)
    QComboBox *m_pattern = nullptr;
    QLineEdit *m_patternValue = nullptr;
    QLabel *m_payloadPreview = nullptr;
    QSpinBox *m_corruptEvery = nullptr;
    QByteArray m_payload;          // current, validated data field
    QString m_payloadDescription;
    bool m_payloadValid = false;

    // Controls
    QPushButton *m_startBtn = nullptr;
    QPushButton *m_stopBtn = nullptr;
    QPushButton *m_exportBtn = nullptr;
    QProgressBar *m_progress = nullptr;

    // Results
    QLabel *m_txRateLbl = nullptr;
    QLabel *m_rxRateLbl = nullptr;
    QLabel *m_lossLbl = nullptr;
    QLabel *m_jitterLbl = nullptr;
    QLabel *m_sentLbl = nullptr;
    QLabel *m_errorsLbl = nullptr;
    QLabel *m_frameErrLbl = nullptr;
    QLabel *m_dataErrLbl = nullptr;

    QTabWidget *m_viewTabs = nullptr;
    RateChart *m_chart = nullptr;
    FrameView *m_frameView = nullptr;
    int m_txSeries = -1;
    int m_rxSeries = -1;
    QTableWidget *m_table = nullptr;
    QPlainTextEdit *m_log = nullptr;

    // Run state
    UdpSender *m_sender = nullptr;   // the UDP engine (core library)
    bool m_running = false;
    QElapsedTimer m_runClock;
    RxStats m_lastRemote;
    bool m_remoteFresh = false;
    TxStats m_txFinal;
    bool m_haveTxFinal = false;
};
