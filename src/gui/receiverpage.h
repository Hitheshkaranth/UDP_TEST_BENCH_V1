#pragma once

#include "protocol.h"

#include <QByteArray>
#include <QWidget>

class FrameView;
class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class RateChart;
class UdpReceiver;

// "Receiver" tab: listener settings, live results, throughput chart, packet structure
// view, table and log. Contains no networking code: listening runs through UdpReceiver.
class ReceiverPage : public QWidget
{
    Q_OBJECT
public:
    explicit ReceiverPage(QWidget *parent = nullptr);
    ~ReceiverPage() override;

private slots:
    void startListening();
    void stopListening();
    void onListening(bool ok, const QString &message);
    void onStopped();
    void onSessionStarted(const QString &peer, const QString &details);
    void onInterval(const RxStats &s);
    void onSessionFinished(const RxStats &s, const QString &reason);
    void onSampleDatagram(const QByteArray &datagram, quint16 srcPort, quint16 dstPort);
    void log(const QString &text);

private:
    QWidget *createListenerBox();
    QWidget *createResultsBox();
    void setListening(bool listening);
    void showTotals(const RxStats &s);

    QComboBox *m_bindAddr = nullptr;
    QSpinBox *m_port = nullptr;
    QCheckBox *m_validate = nullptr;
    QPushButton *m_listenBtn = nullptr;
    QPushButton *m_stopBtn = nullptr;
    QPushButton *m_exportBtn = nullptr;
    QLabel *m_statusLbl = nullptr;

    QLabel *m_peerLbl = nullptr;
    QLabel *m_rateLbl = nullptr;
    QLabel *m_avgLbl = nullptr;
    QLabel *m_receivedLbl = nullptr;
    QLabel *m_lossLbl = nullptr;
    QLabel *m_oooLbl = nullptr;
    QLabel *m_jitterLbl = nullptr;
    QLabel *m_dataErrLbl = nullptr;
    QLabel *m_frameErrLbl = nullptr;

    RateChart *m_chart = nullptr;
    FrameView *m_frameView = nullptr;
    int m_rxSeries = -1;
    QTableWidget *m_table = nullptr;
    QPlainTextEdit *m_log = nullptr;

    UdpReceiver *m_receiver = nullptr;   // the UDP engine (core library)
};
